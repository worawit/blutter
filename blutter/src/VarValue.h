#pragma once
#include "DartClass.h"
#include "DartStub.h"
#include "Util.h"
#include "Disassembler.h"

struct VarStorage {
	enum Kind : int32_t {
		Expression = 0,
		Register,
		Local,
		Argument, // caller argument
		Static, // static variable
		Pool, // object in pool
		Thread,
		InInstruction, // temporary storage when one assembly instruction is splitted to multiple intermediate instruction
		Immediate,
		SmallImm, // array/object offset
		Call, // return value
		Field, // access field
		Uninit,
	};
	VarStorage(A64::Register reg) : kind{ Register }, reg{ reg } {}
	VarStorage(Kind kind) : kind{ kind }, offset{ 0 } {}
	VarStorage(Kind kind, int val) : kind{ kind }, offset{ val } {}

	static VarStorage NewExpression() { return VarStorage(Expression); }
	static VarStorage NewRegister(A64::Register reg) { return VarStorage(reg); }
	static VarStorage NewLocal(int offset) { return VarStorage(Local, offset); }
	static VarStorage NewArgument(int idx) { return VarStorage(Argument, idx); }
	static VarStorage NewStatic(int offset) { return VarStorage(Static, offset); }
	static VarStorage NewPool(int offset) { return VarStorage(Pool, offset); }
	static VarStorage NewThread(int offset) { return VarStorage(Thread, offset); }
	static VarStorage NewImmediate() { return VarStorage(Immediate); }
	static VarStorage NewSmallImm(int val) { return VarStorage(SmallImm, val); }
	static VarStorage NewCall() { return VarStorage(Call); }
	static VarStorage NewUninit() { return VarStorage(Uninit); }

	bool operator==(A64::Register reg) const { return kind == Register && reg == this->reg; }
	bool IsImmediate() { return kind == Immediate; }
	bool IsPredefinedValue() { return kind == Immediate || kind == Pool; }

	std::string Name();

	Kind kind;
	union {
		A64::Register reg;
		int offset; // offset of Local, Pool, Thread, Offset
		int idx; // index of Argument
	};
};

using ValueType = int32_t;

struct VarInteger;
struct VarValue {
	// use Dart class id to determine what the variable type is
	// custom type use negative value
	enum CustomTypeId : int32_t {
		Expression = -1,
		TaggedCid = -2,
		NativeInt = -3,
		NativeDouble = -4,
	};

	VarValue(ValueType typeId, bool hasValue) : typeId(typeId), hasValue(hasValue) {}
	//VarValue() : kind(Unknown), hasValue(false) {}
	virtual ~VarValue() {}
	virtual std::string ToString() = 0;// { return "unknown"; }
	bool HasValue() { return hasValue; }
	virtual ValueType TypeId() { return typeId; }
	ValueType RawTypeId() { return typeId; }

	void SetIntType(ValueType tid);
	void SetSmiIfInt();
	VarInteger* AsInteger() {
		ASSERT(RawTypeId() == dart::kIntegerCid);
		return reinterpret_cast<VarInteger*>(this);
	}

	ValueType typeId;
	bool hasValue;
};

struct VarNull : public VarValue {
	explicit VarNull() : VarValue(dart::kNullCid, true) {}
	virtual std::string ToString() { return "Null"; }
};

struct VarBoolean : public VarValue {
	explicit VarBoolean(bool val) : VarValue(dart::kBoolCid, true), val(val) {}
	explicit VarBoolean() : VarValue(dart::kBoolCid, false), val(false) {}
	virtual std::string ToString() { return val ? "true" : "false"; }

	bool val;
};

struct VarInteger : public VarValue {
	// int type is same as type in VarValue
	// Note: VarInteger = unknown integer type (maybe native, smi, mint)
	explicit VarInteger(int64_t val, ValueType intTypeId = dart::kIntegerCid) : VarValue(dart::kIntegerCid, true), intTypeId(intTypeId), val(val) {}
	explicit VarInteger(ValueType intTypeId = dart::kIntegerCid) : VarValue(dart::kIntegerCid, false), intTypeId(intTypeId), val(0) {}
	virtual std::string ToString() { return std::to_string(Value()); }
	int64_t Value() { return (intTypeId == dart::kSmiCid) ? val >> dart::kSmiTagSize : val; }

	ValueType intTypeId;
	int64_t val;
};

struct VarDouble : public VarValue {
	explicit VarDouble(double val, ValueType doubleTypeId = dart::kDoubleCid) : VarValue(dart::kDoubleCid, true), doubleTypeId(doubleTypeId), val(val) {}
	explicit VarDouble(ValueType doubleTypeId = dart::kDoubleCid) : VarValue(dart::kDoubleCid, false), doubleTypeId(doubleTypeId), val(0.0) {}
	virtual std::string ToString() { return std::to_string(val); }

	ValueType doubleTypeId;
	double val;
};

struct VarString : public VarValue {
	explicit VarString(std::string str) : VarValue(dart::kStringCid, true), str(std::move(str)) {}
	explicit VarString() : VarValue(dart::kStringCid, false) {}
	virtual std::string ToString() { return Util::UnescapeWithQuote(str.c_str()); }

	std::string str;
};

struct VarStub : public VarValue {
	explicit VarStub(DartStub& stub) : VarValue(dart::kFunctionCid, true), stub(stub) {}
	virtual std::string ToString() { return stub.FullName(); }

	DartStub& stub;
};

struct VarField : public VarValue {
	explicit VarField(DartField& field) : VarValue(dart::kFieldCid, true), field(field) {}
	virtual std::string ToString() { return field.Name(); }

	DartField& field;
};

struct VarExpression : public VarValue {
	explicit VarExpression(std::string txt) : VarValue(Expression, false), txt(std::move(txt)), cid(dart::kIllegalCid) {}
	explicit VarExpression(std::string txt, ValueType cid) : VarValue(Expression, false), txt(std::move(txt)), cid(cid) {}
	virtual std::string ToString() { return txt; }
	void SetText(std::string txt) { this->txt = std::move(txt); }
	virtual ValueType TypeId() { return cid; }
	void SetType(ValueType cid) { this->cid = cid; }

	std::string txt;
	ValueType cid;
};

struct VarArray : public VarValue {
	explicit VarArray(dart::ArrayPtr ptr) : VarValue(dart::kArrayCid, true), ptr(ptr), eleType(nullptr), length(-1) {}
	explicit VarArray(DartAbstractType* eleType, int length = -1) : VarValue(dart::kArrayCid, false), ptr(dart::Object::null()), eleType(eleType), length(length) {}
	explicit VarArray() : VarValue(dart::kArrayCid, false), ptr(dart::Object::null()), eleType(nullptr), length(-1) {}
	virtual std::string ToString();
	int64_t DataOffset() {
		// TODO: typedArray has no type argument. so, offset is not the same
		return dart::Array::data_offset();
	}
	int ElementSize() {
		// TODO: typedArray has fixed size
		return dart::kCompressedWordSize;
	}
	bool IsElementTypeInt() {
		//return eleType && eleType->Class().Name() == "int";
		return eleType && eleType->AsType()->Class().Name() == "int";
	}

	dart::ArrayPtr ptr;
	DartAbstractType* eleType;
	int length; // -1 for unknown or growable array
};

// should growable array be treated as instance?
// growable array is not Array
// its data is a Fixed size Array (length of fixed size array is capacity)
struct VarGrowableArray : public VarValue {
	explicit VarGrowableArray(DartAbstractType* eleType) : VarValue(dart::kGrowableObjectArrayCid, false), eleType(eleType) {}
	explicit VarGrowableArray() : VarValue(dart::kGrowableObjectArrayCid, false), eleType(nullptr) {}
	virtual std::string ToString() { return "GrowableArray"; }

	int ElementSize() {
		// TODO: typedArray has fixed size
		return dart::kCompressedWordSize;
	}
	bool IsElementTypeInt() {
		//return eleType && eleType->Class().Name() == "int";
		return eleType && eleType->AsType()->Class().Name() == "int";
	}

	int64_t LengthOffset() {
		return dart::GrowableObjectArray::length_offset();
	}

	int64_t DataOffset() {
		return dart::GrowableObjectArray::data_offset();
	}

	DartAbstractType* eleType;
};

struct VarUnlinkedCall : public VarValue {
	explicit VarUnlinkedCall(DartStub& stub) : VarValue(dart::kUnlinkedCallCid, true), stub(stub) {}
	virtual std::string ToString() { return std::format("UnlinkedCall_{:#x}", stub.Address()); }

	DartStub& stub;
};

// object instance
struct VarInstance : public VarValue {
	explicit VarInstance(DartClass* cls) : VarValue(dart::kInstanceCid, true), cls(cls) {}
	explicit VarInstance() : VarValue(dart::kInstanceCid, false), cls(nullptr) {}
	virtual ValueType TypeId() { return cls->Id(); }
	virtual std::string ToString() { return std::format("Instance_{}", cls->Name()); }

	DartClass* cls;
	//TODO: TypeArguments;
};

struct VarType : public VarValue {
	explicit VarType(const DartType& type) : VarValue(dart::kTypeCid, true), type(type) {}
	virtual std::string ToString() { return type.ToString(); }

	const DartType& type;
};

#ifdef HAS_RECORD_TYPE
struct VarRecordType : public VarValue {
	explicit VarRecordType(const DartRecordType& recordType) : VarValue(dart::kRecordTypeCid, true), recordType(recordType) {}
	virtual std::string ToString() { return recordType.ToString(); }

	const DartRecordType& recordType;
};
#endif

struct VarTypeParameter : public VarValue {
	explicit VarTypeParameter(const DartTypeParameter& typeParam) : VarValue(dart::kTypeParameterCid, true), typeParam(typeParam) {}
	virtual std::string ToString() { return typeParam.ToString(); }

	const DartTypeParameter& typeParam;
};

struct VarFunctionType : public VarValue {
	explicit VarFunctionType(const DartFunctionType& fnType) : VarValue(dart::kFunctionTypeCid, true), fnType(fnType) {}
	virtual std::string ToString() { return fnType.ToString(); }

	const DartFunctionType& fnType;
};

struct VarTypeArgument : public VarValue {
	explicit VarTypeArgument(const DartTypeArguments& typeArgs) : VarValue(dart::kTypeArgumentsCid, true), typeArgs(typeArgs) {}
	virtual std::string ToString() { return typeArgs.ToString(); }

	const DartTypeArguments& typeArgs;
};

// uninitialized object in dart
struct VarSentinel : public VarValue {
	explicit VarSentinel() : VarValue(dart::kSentinelCid, false) {}
	virtual std::string ToString() { return "Sentinel"; }
};

struct VarSubtypeTestCache : public VarValue {
	explicit VarSubtypeTestCache() : VarValue(dart::kSubtypeTestCacheCid, false) {}
	virtual std::string ToString() { return "SubtypeTestCache"; }
};

// A special integer type to represent class id
// cid might be used tagged cid (SMI)
struct VarCid : public VarValue {
	explicit VarCid(int cid, bool isSmi) : VarValue(dart::kClassCid, cid != 0), isSmi(isSmi), cid(cid) {}
	explicit VarCid() : VarValue(dart::kClassCid, false), isSmi(false), cid(0) {}
	virtual std::string ToString() { return isSmi ? std::format("TaggedCid_{}", cid >> dart::kSmiTagSize) : std::format("cid_{}", cid); }

	bool isSmi;
	int cid;
};


struct VarItem {
	explicit VarItem() : storage(VarStorage::Uninit) {}
	explicit VarItem(VarStorage storage) : storage(storage) {}
	explicit VarItem(VarStorage storage, std::shared_ptr<VarValue> val) : storage(storage), val(val) {}
	explicit VarItem(VarStorage storage, VarValue* val) : storage(storage), val(std::shared_ptr<VarValue>(val)) {}
	// register storage is common and also special type
	explicit VarItem(A64::Register reg, std::shared_ptr<VarValue> val) : storage(VarStorage(reg)), val(val) {}
	explicit VarItem(A64::Register reg, VarValue* val) : storage(VarStorage(reg)), val(std::shared_ptr<VarValue>(val)) {}

	VarStorage Storage() { return storage; }
	std::string StorageName() { return storage.Name(); }

	template <typename T, typename = std::enable_if<std::is_base_of<VarValue, T>::value>>
	T* Get() const { return reinterpret_cast<T*>(val.get()); }
	std::shared_ptr<VarValue> Value() const { return val; }
	std::string ValueString() { return val ? val->ToString() : "BUG_NO_ASSIGN_VALUE"; }
	ValueType ValueTypeId() const { return val->RawTypeId(); }
	VarItem* MoveTo(VarStorage storage) { return new VarItem(storage, this->val); }
	VarItem* MoveTo(A64::Register reg) { return new VarItem(VarStorage::NewRegister(reg), this->val); }

	// TODO: more clever name or value when it is known type
	std::string Name();
	std::string CallArgName();

	VarStorage storage;
	//VarType type;
	std::shared_ptr<VarValue> val;
};

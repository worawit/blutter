#pragma once
#include "DartFnBase.h"
#include "CodeAnalyzer.h"

class DartClass;
class DartApp;
struct VarItem;

struct FnParam {
	DartAbstractType* type{ nullptr };
	std::string name;
	bool isRequired{ false };

	explicit FnParam(DartAbstractType* type, std::string name, bool isRequired)
		: type(type), name(std::move(name)), isRequired(isRequired) {}
};

struct DartFunctionSignature
{
	DartAbstractType* ReturnType() const { return returnType; }

	int NumParam() const { return params.size(); }
	int NumOptionalParam() const { return numOptionalParam; }
	bool HasNamedParam() const { return hasNamedParam; }
	std::vector<FnParam>& Params() { return params; }
	FnParam& Param(int i) { return params[i]; }

	DartAbstractType* returnType;
	//typeParams;
	std::vector<FnParam> params;
	int numOptionalParam;
	bool hasNamedParam;
};

class DartFunction : public DartFnBase
{
public:
	enum FunctionKind {
		NORMAL,
		CONSTRUCTOR,
		GETTER,
		SETTER,
	};
	explicit DartFunction(DartClass& cls, const dart::FunctionPtr ptr);
	// for creating naked code (only used for obfuscated app)
	explicit DartFunction(DartClass& cls, const dart::Code& code);
	DartFunction() = delete;
	DartFunction(const DartFunction&) = delete;
	DartFunction(DartFunction&&) = delete;
	DartFunction& operator=(const DartFunction&) = delete;
	virtual ~DartFunction() {}

	DartClass& Class() const { return cls; }
	dart::FunctionPtr Ptr() const { return ptr; }
	//uint64_t Address() { return ep_addr; }
	//uint32_t Size() { return code_size; }
	FunctionKind Kind() const { return kind; }

	uint64_t PayloadAddress() const { return payload_addr; }
	uint64_t PayloadSize() const { return size; }
	uint64_t MonomorphicAddress() const { return morphic_addr; }
	bool HasMorphicCode() const { return morphic_addr != ep_addr; }

	virtual int64_t Size() const { return size > 0 ? size - (ep_addr - payload_addr) : 0; }
	virtual std::string FullName() const;

	DartFunction* GetOutermostFunction() const;

	bool IsNative() const   { return is_native; }
	bool IsClosure() const  { return is_closure; }
	bool IsFfi() const      { return is_ffi; }
	bool IsStatic() const   { return is_static; }
	bool IsConst() const    { return is_const; }
	bool IsAbstract() const { return is_abstract; }
	bool IsAsync() const    { return is_async; }

	DartFunctionSignature& Signature() { return signature; }
	int NumParam() const { return signature.NumParam(); }
	int FirstParamOffset() const { return NumParam() * sizeof(void*) + sizeof(void*); }
	int NumOptionalParam() const { return signature.NumOptionalParam(); }
	bool HasNamedParam() const { return signature.HasNamedParam(); }
	std::vector<FnParam>& Params() { return signature.Params(); }
	FnParam& Param(int i) { return signature.Param(i); }

	void SetAnalyzedData(std::unique_ptr<AnalyzedFnData> data);
	AnalyzedFnData* GetAnalyzedData() { return analyzedData.get(); }

	std::string ToCallStatement(const std::vector<std::shared_ptr<VarItem>>& args) const;
	void PrintHead(std::ostream& of) const;
	void PrintFoot(std::ostream& of) const;

private:
	DartClass& cls;
	DartFunction* parent; // this value is nullptr for function. parent function/closure for a closure
	dart::FunctionPtr ptr;
	FunctionKind kind;
	bool is_native;
	bool is_closure;
	bool is_ffi; // if a function is FFI, no use of is_static, is_const, is_abstract
	bool is_static;
	bool is_const;
	bool is_abstract;
	bool is_async;

	uint64_t payload_addr; // the start of whole function data (most of them are same as entry point)
	uint64_t morphic_addr; // Monomorphic entry point (used for check class id before normal entry point)
	//uint32_t code_size; // code size

	DartFunctionSignature signature;
	std::unique_ptr<AnalyzedFnData> analyzedData;

	friend class DartApp;
};


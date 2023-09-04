#pragma once
#include <string>
#include "DartField.h"

class DartLibrary;
class DartFunction;

class DartClass
{
public:
	enum ClassType {
		CLASS,    // [normal] class
		ABSTRACT, // abstract class
		ENUM,     // enum (parent class is _Enum)
	};
	explicit DartClass(const DartLibrary& lib, const dart::Class& cls);
	// for creating dummy class (only used for obfuscated app)
	explicit DartClass(const DartLibrary& lib);
	DartClass() = delete;
	DartClass(const DartClass&) = delete;
	DartClass(DartClass&&) = delete;
	DartClass& operator=(const DartClass&) = delete;
	~DartClass();

	DartFunction* AddFunction(const dart::ObjectPtr funcPtr);
	DartFunction* AddFunction(const dart::Code& code);
	DartField* AddField(const dart::ObjectPtr fieldPtr);
	DartField* AddField(intptr_t offset, DartAbstractType* type, bool nativeNumber = false);
	DartField* FindField(intptr_t offset);

	//bool IsNative() { return lib.ptr == nullptr; }
	bool IsTopClass() const { return dart::ClassTable::IsTopLevelCid(id); }
	uint32_t Id() const { return id; }
	const DartLibrary& Library() const { return lib; }
	dart::ClassPtr Ptr() const { return ptr; }
	DartClass* Parent() const { return superCls; }

	DartType* DeclarationType() { return declarationType; }

	const std::string& Name() const { return name; }
	std::string FullName() const { return name + typeVectorName; }
	std::string FullNameWithPackage() const;

	uint32_t NumTypeArguments() const { return num_type_arguments; }
	uint32_t NumTypeParameters() const { return num_type_parameters; }
	bool HasTypeArguments() const { return type_argument_offset != dart::Class::kNoTypeArguments; }
	int32_t TypeArgumentsOffset() const { return type_argument_offset; }

	int32_t Size() const { return size; }
	uint64_t FieldBitmap() const { return unboxed_fields_bitmap.Value(); }
	dart::UnboxedFieldBitmap UnboxedFieldsBitmap() const { return unboxed_fields_bitmap; }
	int32_t TypeArgumentOffset() const { return type_argument_offset; }

	std::vector<DartField*>& Fields() { return fields; }
	std::vector<DartFunction*>& Functions() { return functions; };

	void PrintHead(std::ostream& of);
	void PrintFoot(std::ostream& of);

private:
	const DartLibrary& lib;
	dart::UnboxedFieldBitmap unboxed_fields_bitmap;
	uint32_t id; // class id
	//uint32_t superCid; // parent class id
	DartClass* superCls; // super class, initialized as NULL, set after all classes are loaded
	const dart::ClassPtr ptr;
	DartType* declarationType;
	std::string name;
	std::string typeVectorName; // <type parameters>
	std::string parentTypeVectorName; // <type parameters> of parent class for this class
	ClassType type;
	//uint32_t parent_id;
	// num_type_arguments is declaration_args length
	// it is args in <>. the number is from number of this class and the parent
	//DartTypeArguments* declaration_args;
	uint32_t num_type_arguments;
	// num_type_params is this length (number of this class arguments)
	uint32_t num_type_parameters;
	//DartTypeParametersItem* type_params;
	std::vector<DartClass*> interfaces;
	DartClass* mixin;
	//uint32_t parent_size; // offset to start of this class fields
	int32_t type_argument_offset;
	int32_t size;
	bool is_const_constructor;
	bool is_transformed_mixin;
	std::vector<DartField*> fields;
	std::vector<DartFunction*> functions;

	friend class DartApp;
};


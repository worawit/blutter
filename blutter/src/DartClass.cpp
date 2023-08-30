#include "pch.h"
#include "DartClass.h"
#include "DartLibrary.h"
#include "DartFunction.h"
#include "HtArrayIterator.h"
#include <numeric>

DartClass::DartClass(const DartLibrary& lib_, const dart::Class& cls) :
	lib(lib_), unboxed_fields_bitmap(0), superCls(nullptr), ptr(cls.ptr()), declarationType(nullptr), type(CLASS), 
	num_type_arguments(0), num_type_parameters(0), mixin(nullptr), is_const_constructor(false), is_transformed_mixin(false)
{
	auto zone = dart::Thread::Current()->zone();

	ASSERT(cls.is_type_finalized());

	id = (uint32_t)cls.id();
	// empty string for top class (default is "::")
	if (!cls.IsTopLevel()) {
		// Note: Dart use "Object" as instance name because it is parent of all class
		name = cls.ScrubbedNameCString();
	}

	// host_instance_size() is allocated size from heap (need alignment)
	// we need only exact size to know the offset of subclass members
	size = (int32_t)cls.host_next_field_offset();
	type_argument_offset = (int32_t)cls.host_type_arguments_field_offset();
	//const auto& supCls = Class::Handle(zone, cls.SuperClass()); // parent class

	// sizeof(UntaggedObject) == sizeof(uword);  // 32 or 64 bits depended on architecture
	// UntaggedObject.HeapSize() includes sizeof(UntaggedObject)

	if (id == dart::kGrowableObjectArrayCid) {
		//auto dartField = new DartField(*this, dart::Field::RawCast(fieldPtr));
		//fields.push_back(dartField);
	}

	if (!cls.is_loaded() || id <= dart::kLastInternalOnlyCid) {
		// can assume the class is native type (also no parent class)
		// there is no info (except class name) for native type.
		return;
	}

	if (!dart::ClassTable::IsTopLevelCid(id)) {
		//auto& supCls = dart::Class::Handle(zone, cls.SuperClass());
		auto supClsPtr = cls.SuperClass();
		
		auto superCid = supClsPtr.untag()->id();
		if (superCid > 0 && (intptr_t)supClsPtr == (intptr_t)dart::Object::null())
			superCid = 0;
		if (superCid)
			superCls = (DartClass*)(intptr_t)superCid; // temporary save class id as pointer. it will be set correctly after all classes are loaded

		if (cls.is_const())
			is_const_constructor = true;
		// from "vm/class_finalizer.cc":"markImplemented()"
		// For a class used as an interface marks this class and all its superclasses implemented.
		// Note: mixin is a interface too
		// 
		// the class is auto generated from user class with mixin(s)
		// to find the real parent class (after "extends" keyword),
		//   we have to follow up the parent class until class has no mixin
		// the last usage class has no mixin
		// Note: the name of this class is prefixed with "_<child class name>&<extends>&<mixin>"
		is_transformed_mixin = cls.is_transformed_mixin_application();

		// In AOT, all class is finalized
		//cls.is_finalized()
		if (cls.is_abstract())
			type = ABSTRACT;
		else if (cls.is_enum_class())
			type = ENUM;

		num_type_parameters = (uint32_t)cls.NumTypeParameters();
		num_type_arguments = (uint32_t)cls.NumTypeArguments();
		// parent class is needed to generete subvector type parameters of its parent, delay until superCls is set
	}
	// interfaces reference to other types. wait until all classes are loaded

	// where is nested class?
	{
		const auto& fields = dart::Array::Handle(zone, cls.fields());
		intptr_t num = fields.Length();
		for (intptr_t i = 0; i < num; i++) {
			auto fieldPtr = fields.At(i);
			AddField(fieldPtr);
		}
	}

	{
		const auto& funcs = dart::Array::Handle(zone, cls.functions());
		intptr_t num_funcs = funcs.Length();
		for (intptr_t i = 0; i < num_funcs; i++) {
			auto funcPtr = funcs.At(i);
			AddFunction(funcPtr);
		}
	}

	//{
	//	// Canonicalized const instances of this class (UntaggedClass)
	//	// constants is HashTable. constants.Length() is array length (number of slots), not number of const instances
	//	const auto& constants = dart::Array::Handle(zone, cls.constants());
	//	if (constants.ptr() != dart::Object::null_array().ptr()) {
	//		// TODO:
	//		auto& obj = dart::Object::Handle(zone);
	//		HtArrayIterator it(constants);
	//		while (it.MoveNext()) {
	//			const auto obj_ptr = it.Current();
	//			ASSERT(obj_ptr.GetClassId() == id);
	//			obj = obj_ptr;
	//			if (id == dart::kImmutableArrayCid) {
	//			}
	//			else if (id == dart::kDoubleCid) {
	//				auto val = dart::Double::Cast(obj).value();
	//				//std::cout << std::format("  const double: {}\n", val);
	//			}
	//			else if (id == dart::kMintCid) {
	//				auto val = dart::Mint::Cast(obj).value();
	//				//std::cout << std::format("  const Mint: {:#x}\n", val);
	//			}
	//			else if (id == dart::kConstMapCid) {
	//				auto& map = dart::Map::Cast(obj);
	//			}
	//			else if (id == dart::kInstanceCid) {
	//				// just one const Object
	//			}
	//			else
	//				id = id;
	//		}
	//	}
	//}
}

DartClass::~DartClass()
{
	for (auto field : fields) {
		delete field;
	}
	for (auto func : functions) {
		delete func;
	}
}

DartFunction* DartClass::AddFunction(const dart::ObjectPtr funcPtr)
{
	auto dartFn = new DartFunction(*this, dart::Function::RawCast(funcPtr));
	functions.push_back(dartFn);
	return dartFn;
}

DartField* DartClass::AddField(const dart::ObjectPtr fieldPtr)
{
	auto dartField = new DartField(*this, dart::Field::RawCast(fieldPtr));
	fields.push_back(dartField);
	return dartField;
}

DartField* DartClass::AddField(intptr_t offset, DartAbstractType* type, bool nativeNumber)
{
	auto dartField = FindField(offset);
	if (dartField == nullptr) {
		dartField = new DartField(*this, (uint32_t)offset, type);
		fields.push_back(dartField);
	}
	else {
		auto ctype = dartField->Type();
		if (ctype != type) {
			// TODO: make it be type parameter
			//if (ctype->IsNull()) {
			//	dartField->SetType(type);
			//}
			//else if (nativeNumber) {
			//	// always assume double if possible (can be confirmed from register type in assembly)
			//	if (type->Class().Id() == dart::kDoubleCid)
			//		dartField->SetType(type);
			//}
			// TODO: subclass(polymorphic) or type parameter
		}
	}
	return dartField;
}

DartField* DartClass::FindField(intptr_t offset)
{
	auto it = std::find_if(fields.begin(), fields.end(), [offset](const DartField* field) { return field->Offset() == offset; });
	if (it == fields.end())
		return nullptr;
	return *it;
}

std::string DartClass::FullNameWithPackage() const
{
	return "[" + lib.url + "] " + name + typeVectorName;
}

void DartClass::PrintHead(std::ostream& of)
{
	if (superCls == NULL)
		of << std::format("\n// class id: {}, size: {:#x}\n", id, size);
	else
		of << std::format("\n// class id: {}, size: {:#x}, field offset: {:#x}\n", id, size, superCls->size);
	if (dart::ClassTable::IsTopLevelCid(id)) {
		of << "class :: {\n";
		return;
	}
	if (superCls == NULL) {
		of << std::format("class {};\n", name.c_str());
		return;
	}

	if (is_const_constructor || is_transformed_mixin) {
		of << "//   ";
		if (is_const_constructor)
			of << "const constructor, ";
		if (is_transformed_mixin)
			of << "transformed mixin,";
		of << "\n";
	}

	std::string cls_prefix;
	if (type == DartClass::CLASS)
		cls_prefix = "class";
	else if (type == DartClass::ABSTRACT)
		cls_prefix = "abstract class";
	else if (type == DartClass::ENUM)
		cls_prefix = "enum";
	else
		cls_prefix = "maybe_class";

	of << cls_prefix << " ";

	of << name << typeVectorName;

	of << " extends " << superCls->name << parentTypeVectorName;;

	// if there is a mixin, the last one is mixin
	if (!interfaces.empty() || mixin) {
		of << "\n    ";
		if (!interfaces.empty()) {
			of << "implements ";
			of << std::accumulate(interfaces.begin() + 1, interfaces.end(), interfaces[0]->FullName(),
				[](std::string x, DartClass* y) {
					return x + ", " + y->FullName();
				}
			);
		}
		if (mixin) {
			of << " with " << mixin->FullName();
		}
	}
	
	of << " {\n";
}

void DartClass::PrintFoot(std::ostream& of)
{
	of << "}\n";
}

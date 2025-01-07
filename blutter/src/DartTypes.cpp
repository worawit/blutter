#include "pch.h"
#include "DartTypes.h"
#include "DartClass.h"
#include <numeric>
#include <sstream>

const DartTypeArguments DartTypeArguments::Null;

std::string DartTypeArguments::SubvectorName(int from_index, int len) const
{
	ASSERT(len > 0);
	std::string txt;
	txt.reserve(len * 32);
	txt += "<";
	for (auto i = 0; i < len; i++) {
		if (i > 0) {
			txt += ", ";
		}
		const size_t pos = from_index + i;
		if (pos < args.size()) {
			txt += args[pos]->ToString();
		}
		else {
			txt += "dynamic";
		}
	}
	txt += ">";
	return txt;
}

std::string DartType::ToString() const
{
	return ToString(true);
}

std::string DartType::ToString(bool showTypeArgs) const
{
	std::string txt = cls.Name();
	if (showTypeArgs) {
		txt += args->ToString();
	}
	if (IsNullable() && cls.Id() != dart::kDynamicCid) {
		txt += "?";
	}
	return txt;
}

#ifdef HAS_RECORD_TYPE
std::string DartRecordType::ToString() const
{
	std::string txt = "(";
	const intptr_t num_positional_fields = fieldTypes.size() - fieldNames.size();
	for (auto i = 0; i < fieldTypes.size(); i++) {
		if (i != 0) {
			txt += ", ";
		}
		if (i == num_positional_fields) {
			txt += '{';
		}
		txt += fieldTypes[i]->ToString();
		if (i >= num_positional_fields) {
			txt += ' ';
			txt += fieldNames[i - num_positional_fields];
		}
	}
	if (num_positional_fields < fieldTypes.size()) {
		txt += '}';
	}
	txt += ')';
	if (IsNullable())
		txt += '?';
	return txt;
}
#endif

#ifdef HAS_TYPE_REF
std::string DartTypeRef::ToString() const
{
	return type.ToString(false);
}
#endif

std::string DartTypeParameter::ToString() const
{
	// CanonicalName might not start from 0
	std::string txt;
	if (base != 0)
		txt += (isClassTypeParam ? "C" : "F") + std::to_string(base);
	txt += (isClassTypeParam ? "X" : "Y") + std::to_string(index - base);
	if (IsNullable()) {
		txt += "?";
	}
	if (bound->IsType()) {
		auto& cls = bound->AsType()->Class();
		if (cls.Id() != dart::kInstanceCid) {
#ifdef HAS_TYPE_REF
			txt += " bound " + bound->ToString();
#else
			txt += " bound " + bound->AsType()->ToString(false);
#endif
		}
	}
	return txt;
}

std::string DartFunctionType::ToString() const
{
	std::string txt;
	if (IsNullable()) {
		txt += "(";
	}

	if (!typeParams.empty()) {
		txt += "<";
		txt += std::accumulate(typeParams.begin() + 1, typeParams.end(), typeParams.front()->ToString(),
			[](std::string x, const DartTypeParameter* y) {
				return x + ", " + y->ToString();
			}
		);
		txt += ">";
	}

	txt += "("; // open for function arguments
	if (!params.empty()) {
		auto first = params[0].type->ToString();
		if (hasImplicitParam)
			first += " this";
		txt += std::accumulate(params.begin() + 1, params.end(), first,
			[](std::string x, const Parameter& y) {
				return x + ", " + y.type->ToString();
			}
		);
	}
	if (!optionalParams.empty()) {
		if (!params.empty()) {
			txt += ", ";
		}
		if (hasNamedParam) {
			txt += std::accumulate(optionalParams.begin() + 1, optionalParams.end(), optionalParams[0].type->ToString() + " " + optionalParams[0].name,
				[](std::string x, const Parameter& y) {
					return x + ", " + y.type->ToString() + " " + y.name;
				}
			);
		}
		else {
			txt += std::accumulate(optionalParams.begin() + 1, optionalParams.end(), optionalParams[0].type->ToString(),
				[](std::string x, const Parameter& y) {
					return x + ", " + y.type->ToString();
				}
			);
		}
	}
	txt += ") => " + resultType->ToString(); // close for function arguments and return type
	if (IsNullable()) {
		txt += ")?";
	}
	return txt;
}

DartType* DartTypeDb::FindOrAdd(dart::TypePtr typePtr)
{
	auto ptr = (intptr_t)typePtr;
	if (typesMap.contains(ptr)) {
		return typesMap[ptr]->AsType();
	}

	const auto& type = dart::Type::Handle(typePtr);
	auto cid = type.type_class_id();
	auto dartCls = classes[type.type_class_id()];
	ASSERT(dartCls);
	// Add it to DB first. the type arguments might be many recursive calls
	auto dartType = new DartType(type.IsNullable(), *dartCls, &DartTypeArguments::Null);
	typesMap[ptr] = dartType;

	dartType->args = FindOrAdd(type.arguments());

	auto& types = typesByCid[dartCls->Id()];
	// can use pointer comparison because we create only one type args for one pointer
	// so different pointer means another type
	auto it = std::find(types.begin(), types.end(), dartType);
	if (it == types.end()) {
		types.push_back(dartType);
	}

	return dartType;
}

#ifdef HAS_RECORD_TYPE
DartRecordType* DartTypeDb::FindOrAdd(dart::RecordTypePtr recordTypePtr)
{
	auto ptr = (intptr_t)recordTypePtr;
	if (typesMap.contains(ptr)) {
		return typesMap[ptr]->AsRecordType();
	}

	auto thread = dart::Thread::Current();
	auto zone = thread->zone();

	const auto& recordType = dart::RecordType::Handle(zone, recordTypePtr);

	std::vector<std::string> fieldNames;
	const auto& field_names = dart::Array::Handle(zone, recordType.GetFieldNames(thread));
	auto& name = dart::String::Handle(zone);
	for (intptr_t i = 0; i < field_names.Length(); i++) {
		name ^= field_names.At(i);
		fieldNames.push_back(name.ToCString());
	}

	auto dartRecordType = new DartRecordType(recordType.IsNullable(), std::move(fieldNames));
	typesMap[ptr] = dartRecordType;

	const auto num_fields = recordType.NumFields();
	for (intptr_t i = 0; i < num_fields; i++) {
		auto abTypePtr = recordType.FieldTypeAt(i);
		dartRecordType->fieldTypes.push_back(FindOrAdd(abTypePtr));
	}

	return dartRecordType;
}
#endif

DartTypeParameter* DartTypeDb::FindOrAdd(dart::TypeParameterPtr typeParamPtr)
{
	auto ptr = (intptr_t)typeParamPtr;
	if (typesMap.contains(ptr)) {
		return typesMap[ptr]->AsTypeParameter();
	}

	const auto& typeParam = dart::TypeParameter::Handle(typeParamPtr);
	// Add it to DB first. the bound might be many recursive calls
	auto dartTypeParam = new DartTypeParameter(typeParam.IsNullable(), (uint16_t)typeParam.base(), (uint16_t)typeParam.index(), typeParam.IsClassTypeParameter());
	typesMap[ptr] = dartTypeParam;

	// Removing TypeRef also replaces TypeParameter.bound with TypeParameter.owner
#ifdef HAS_TYPE_REF
	dartTypeParam->bound = FindOrAdd(typeParam.bound());
#else
	// in Dart 3.5, owner.ptr() might be NULL (zero value)
	// code in TypeParameter::bound() is changed between Dart version, do NOT copy it to here
	//   only add the edge case here
	if ((intptr_t)typeParamPtr.untag()->owner() == 0 || (intptr_t)typeParam.parameterized_class() == 0)
		dartTypeParam->bound = FindOrAdd(dart::Isolate::Current()->group()->object_store()->nullable_object_type());
	else
		dartTypeParam->bound = FindOrAdd(typeParam.bound());
#endif

	return dartTypeParam;
}

DartFunctionType* DartTypeDb::FindOrAdd(dart::FunctionTypePtr fnTypePtr)
{
	auto ptr = (intptr_t)fnTypePtr;
	if (typesMap.contains(ptr)) {
		return typesMap[ptr]->AsFunctionType();
	}

	const auto& fnType = dart::FunctionType::Handle(fnTypePtr);

	// handle function type parameters
	std::vector<DartTypeParameter*> typeParams;
	if (fnType.NumTypeParameters() != 0) {
		const auto& type_params = dart::TypeParameters::Handle(fnType.type_parameters());
		RELEASE_ASSERT(!type_params.IsNull());
		const intptr_t num_type_params = type_params.Length();
		const intptr_t base = fnType.NumParentTypeArguments();
		const bool kIsClassTypeParameter = false;
		for (intptr_t i = 0; i < num_type_params; i++) {
			auto dartTypeParam = new DartTypeParameter(false, (uint16_t)base, (uint16_t)(base + i), kIsClassTypeParameter);
			dartTypeParam->bound = FindOrAdd(type_params.BoundAt(i));
			// Note: there might be defaults to

			typeParams.push_back(dartTypeParam);
		}
	}

	// Add it to DB first. the bound might be many recursive calls
	auto dartFnType = new DartFunctionType(fnType.IsNullable(), fnType.num_implicit_parameters() != 0, fnType.HasOptionalNamedParameters(), std::move(typeParams));
	typesMap[ptr] = dartFnType;

	dartFnType->resultType = FindOrAdd(fnType.result_type());

	const auto num_fixed_params = fnType.num_fixed_parameters();
	dartFnType->params.reserve(num_fixed_params);
	for (auto i = 0; i < num_fixed_params; i++) {
		dartFnType->params.emplace_back("", FindOrAdd(fnType.ParameterTypeAt(i)));
	}

	const auto num_opt_params = fnType.NumOptionalParameters();
	if (num_opt_params > 0) {
		dartFnType->optionalParams.reserve(num_opt_params);
		const auto num_params = num_fixed_params + num_opt_params;
		auto& name = dart::String::Handle();
		const char* tmp = "";
		for (auto i = num_fixed_params; i < num_params; i++) {
			//dartFnType->params[i].type = FindOrAdd(fnType.ParameterTypeAt(i));
			//dartFnType->params[i].name = name.ToCString();
			if (dartFnType->hasNamedParam) {
				name = fnType.ParameterNameAt(i);
				tmp = name.ToCString();
			}
			dartFnType->optionalParams.emplace_back(tmp, FindOrAdd(fnType.ParameterTypeAt(i)), nullptr);
		}
	}

	return dartFnType;
}

DartAbstractType* DartTypeDb::FindOrAdd(dart::AbstractTypePtr abTypePtr)
{
	switch (abTypePtr.GetClassId()) {
	case dart::kTypeCid:
		return FindOrAdd(dart::Type::RawCast(abTypePtr));
#ifdef HAS_RECORD_TYPE
	case dart::kRecordTypeCid:
		return FindOrAdd(dart::RecordType::RawCast(abTypePtr));
#endif
#ifdef HAS_TYPE_REF
	case dart::kTypeRefCid: {
		auto typePtr = dart::TypeRef::RawCast(abTypePtr)->untag()->type();
		ASSERT(typePtr.GetClassId() == dart::kTypeCid);
		return new DartTypeRef(*FindOrAdd(dart::Type::RawCast(typePtr)));
	}
#endif
	case dart::kTypeParameterCid:
		return FindOrAdd(dart::TypeParameter::RawCast(abTypePtr));
	case dart::kFunctionTypeCid:
		return FindOrAdd(dart::FunctionType::RawCast(abTypePtr));
	}
	//return nullptr;
	FATAL("Invalid abstract type");
}

const DartTypeArguments* DartTypeDb::FindOrAdd(dart::TypeArgumentsPtr typeArgsPtr)
{
	if ((intptr_t)typeArgsPtr == (intptr_t)dart::Object::null()) {
		return &DartTypeArguments::Null;
	}

	auto ptr = (intptr_t)typeArgsPtr;
	if (typeArgsMap.contains(ptr)) {
		return typeArgsMap[ptr];
	}

	auto& typeArgs = dart::TypeArguments::Handle(typeArgsPtr);
	const auto typeArgsLen = typeArgs.Length();
	std::vector<DartAbstractType*> args(typeArgsLen);

	// Add it to DB first. the bound might be many recursive calls
	auto dartTypeArgs = new DartTypeArguments(std::move(args));
	typeArgsMap[ptr] = dartTypeArgs;

	for (auto i = 0; i < typeArgsLen; i++) {
		const auto abTypePtr = typeArgs.TypeAt(i);
		dartTypeArgs->args[i] = FindOrAdd(abTypePtr);
	}

	return dartTypeArgs;
}

DartType* DartTypeDb::FindOrAdd(DartClass& dartCls, const dart::TypeArgumentsPtr typeArgsPtr)
{
	auto args = FindOrAdd(typeArgsPtr);
	auto& types = typesByCid[dartCls.Id()];
	// we want to find same type args
	// so different pointer means another type
	DartType* dartType;
	auto it = std::find_if(types.begin(), types.end(), [&args](const DartType* dtype) {
		return dtype->args == args;
	});
	if (it == types.end()) {
		dartType = new DartType{ false, dartCls, args };
		types.push_back(dartType);
	}
	else {
		dartType = *it;
	}

	return dartType;
}

DartType* DartTypeDb::FindOrAdd(DartClass& dartCls, const dart::Instance& inst)
{
	if (dartCls.NumTypeParameters() == 0) {
		// this class cannot be parameterized
		return dartCls.DeclarationType();
	}

	// the instance always has type arguments because the class can be parameterized
	return FindOrAdd(dartCls, inst.GetTypeArguments());
}


DartType* DartTypeDb::FindOrAdd(uint32_t cid, const DartTypeArguments* typeArgs)
{
	for (auto type : typesByCid[cid]) {
		if (type->args == typeArgs)
			return type;
	}
	auto dartType = new DartType{ false, *classes[cid], typeArgs };
	typesByCid[cid].push_back(dartType);
	return dartType;
}

DartType* DartTypeDb::Get(uint32_t cid)
{
	auto dartCls = classes[cid];
	ASSERT(dartCls->NumTypeParameters() == 0);
	return dartCls->DeclarationType();
}

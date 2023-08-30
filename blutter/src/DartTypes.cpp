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

std::string DartTypeRef::ToString() const
{
	return type.ToString(false);
}

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
			txt += " bound " + bound->ToString();
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
	dartTypeParam->bound = FindOrAdd(typeParam.bound());

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
	case dart::kTypeRefCid: {
		//auto& typeRef = dart::TypeRef::Handle(dart::TypeRef::RawCast(abTypePtr));
		//auto typePtr = typeRef.type();
		auto typePtr = dart::TypeRef::RawCast(abTypePtr)->untag()->type();
		ASSERT(typePtr.GetClassId() == dart::kTypeCid);
		return new DartTypeRef(*FindOrAdd(dart::Type::RawCast(typePtr)));
	}
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
		dartType = new DartType(false, dartCls, args);
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

DartType* DartTypeDb::Get(uint32_t cid)
{
	auto dartCls = classes[cid];
	ASSERT(dartCls->NumTypeParameters() == 0);
	return dartCls->DeclarationType();
}

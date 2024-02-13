#include "pch.h"
#include "DartFunction.h"
#include "DartClass.h"
#include "DartLibrary.h"
#include "DartApp.h"
#include "VarValue.h"
#include "CodeAnalyzer.h"
#include <numeric>
#include <array>

// place static member because no DartFnBase source file
intptr_t DartFnBase::lib_base;

DartFunction::DartFunction(DartClass& cls, const dart::FunctionPtr ptr) : DartFnBase(), cls(cls), parent(nullptr), ptr(ptr), kind(NORMAL)
{
	auto* zone = dart::Thread::Current()->zone();

	const auto& func = dart::Function::Handle(zone, ptr);

	// might need internal name for complete getter and setter name
	name = func.UserVisibleNameCString();

	is_native = func.is_native();
	//is_closure = name == "<anonymous closure>";
	is_closure = func.IsClosureFunction(); //func.IsNonImplicitClosureFunction();
	//ASSERT(is_closure == (name == "<anonymous closure>"));
	// Note: https://github.com/dart-lang/sdk/commit/dcdfcc2b8decc8bf47881f2e93ee5503ea98b7cf#diff-fba8499e9b9e86f518a0ad80eeda6a4309ac5a82c5e9e0b0bf962505e1ecddba
	//   IsFfiTrampoline() is changed to IsFfiCallbackTrampoline()
	is_ffi = func.kind() == dart::UntaggedFunction::kFfiTrampoline;
	if (!is_ffi) {
		//is_static = func.IsStaticFunction();
		is_static = func.is_static();
		is_const = func.is_const();
		is_abstract = func.is_abstract();
		is_async = func.IsAsyncFunction();
		func.IsGetterFunction();

		// function attributes
		switch (func.kind()) {
		case dart::UntaggedFunction::kConstructor:
			kind = CONSTRUCTOR;
			break;
		case dart::UntaggedFunction::kSetterFunction:
		case dart::UntaggedFunction::kImplicitSetter:
			kind = SETTER;
			break;
		case dart::UntaggedFunction::kGetterFunction:
		case dart::UntaggedFunction::kImplicitGetter:
		case dart::UntaggedFunction::kImplicitStaticGetter:
		//case dart::UntaggedFunction::kRecordFieldGetter:
			kind = GETTER;
			break;
		default:
			kind = NORMAL;
		}
	}
	else {
		is_static = false;
		is_const = false;
		is_abstract = false;
		is_async = false;
		//auto fnType = func.FfiCSignature();
		//if (!fnType.IsRawNull()) {
		//	auto& sig = dart::FunctionType::Handle(zone, fnType);
		//	std::cout << "FFI: " << name << ", sig: " << sig.ToCString() << "\n";
		//}
	}

	// the generated code can be checked from Assembler::MonomorphicCheckedEntryAOT()
	// Code.EntryPoint() in obfuscated app might be pointed at start of snapshot (wrong)
	const auto ep = func.entry_point() - lib_base;
	const auto& code = dart::Code::Handle(zone, func.CurrentCode());
	payload_addr = code.PayloadStart();
	if (payload_addr > 0)
		payload_addr -= lib_base;
	morphic_addr = code.MonomorphicEntryPoint();
	if (morphic_addr > 0)
		morphic_addr -= lib_base;
	size = code.Size();
	ep_addr = code.EntryPoint() - lib_base;
	if (ep != ep_addr) {
		ep_addr = ep;
		//std::cout << std::format("Fn: {}, payload: {:#x}, ep_addr: {:#x}, ep: {:#x}\n", name.c_str(), payload_addr, ep_addr, ep);
	}

	//if (ep_addr != payload_addr) {
	//	std::cout << std::format("Fn: {}, payload: {:#x}, morphic: {:#x}, ep: {:#x}\n", name.c_str(), payload_addr, morphic_addr, ep_addr);
	//}

	// closure parent (what function use this closure)
	if (is_closure) {
		is_static = func.is_static();
		auto parentPtr = func.parent_function();
		if ((intptr_t)parentPtr != (intptr_t)dart::Function::null()) {
			//auto outmost_parent = func.GetOutermostFunction();
			//auto& parentFn = dart::Function::Handle(zone, parentPtr);
			// Now, the parent function might be missing.
			//   store parent as entry point first. it will be changed to pointer later.
			//parent = (DartFunction*)(parentFn.entry_point() - lib_base);
			parent = (DartFunction*)(intptr_t)parentPtr;
		}
		else {
			//std::cout << std::format("[?] closure {} ({:#x}) has null parent\n", name, ep_addr);
		}
	}

	// TODO:
	// more info: https://mrale.ph/dartvm/compiler/exceptions.html
	//auto& catchData = dart::TypedData::Handle(zone, code.catch_entry_moves_maps());
}

// kind should be raw code
DartFunction::DartFunction(DartClass& cls, const dart::Code& code)
	: DartFnBase(), cls(cls), parent(nullptr), ptr(dart::Function::null()), kind(NORMAL),
	is_native(true), is_closure(false), is_ffi(false), is_static(false), is_const(false), is_abstract(false), is_async(false)
{
	payload_addr = code.PayloadStart();
	if (payload_addr > 0)
		payload_addr -= lib_base;
	morphic_addr = code.MonomorphicEntryPoint();
	if (morphic_addr > 0)
		morphic_addr -= lib_base;
	size = code.Size();
	ep_addr = code.EntryPoint() - lib_base;
	name = "__unknown_function__";
}

std::string DartFunction::FullName() const
{
	auto& lib = cls.Library();
	return "[" + lib.url + "] " + cls.Name() + "::" + name;
}

DartFunction* DartFunction::GetOutermostFunction() const
{
	// Only closure should call this method
	if (!parent)
		return nullptr;

	DartFunction* topFn = parent;
	while (topFn->parent) {
		ASSERT(topFn->IsClosure());
		topFn = topFn->parent;
	}
	return topFn;
}

void DartFunction::SetAnalyzedData(std::unique_ptr<AnalyzedFnData> data)
{
	// must never be called more than once
	ASSERT(!analyzedData);
	
	analyzedData = std::move(data);
}

static const auto BinaryOpNames = std::to_array<std::string>({ "+", "-", "*", "/", "~/", "%", "&", "|" });
static bool isBinaryOpName(const std::string& name)
{
	return std::find(BinaryOpNames.begin(), BinaryOpNames.end(), name) != BinaryOpNames.end();
}

std::string DartFunction::ToCallStatement(const std::vector<std::shared_ptr<VarItem>>& args) const
{
	std::string callArgs = "";
	std::string callFn = "";
	if (IsStatic() || kind == CONSTRUCTOR) {
		if (!args.empty()) {
			callArgs = std::accumulate(args.begin() + 1, args.end(), args[0]->CallArgName(),
				[](std::string x, const std::shared_ptr<VarItem> y) {
					return x + ", " + y->CallArgName();
				}
			);
		}
		callFn = Name();
	}
	else {
		ASSERT(!args.empty());
		if (isBinaryOpName(name)) {
			ASSERT(args.size() == 2);
			return args[0]->CallArgName() + " " + name + " " + args[1]->CallArgName();
		}
		else {
			if (args.size() > 1) {
				callArgs = std::accumulate(args.begin() + 2, args.end(), args[1]->CallArgName(),
					[](std::string x, const std::shared_ptr<VarItem>& y) {
						return x + ", " + y->CallArgName();
					}
				);
			}
			callFn = args[0]->CallArgName() + "." + Name();
		}
	}
	return std::format("{}({})", callFn.c_str(), callArgs.c_str());
}

void DartFunction::PrintHead(std::ostream& of) const
{
	//of << std::format("    {} /* addr: {:#x}, size: {:#x} */\n", func.ToCString(), ep, code_size);
	auto zone = dart::Thread::Current()->zone();
	auto& func = dart::Function::Handle(zone, ptr);

	// Note: Signature is not dropped in aot when any named parameter is required. (from Function::IsRequiredAt() body)
	const auto& sig = dart::FunctionType::Handle(zone, func.signature());

	of << "  "; // indentation
	if (is_closure)
		of << "[closure] ";

	if (is_ffi) {
		of << "[ffi] ";
	}
	else {
		if (is_const)
			of << "const ";
		else if (is_abstract)
			of << "abstract ";

		switch (kind) {
		case CONSTRUCTOR:
			if (is_static)
				of << "factory ";
			break;
		case SETTER:
			of << "set ";
			break;
		case GETTER:
			if (sig.IsNull())
				of << "get ";
			break;
		default:
			if (is_static)
				of << "static ";
			break;
		}
	}

	if (sig.IsNull()) {
		of << "_ " << name << "(/* No info */)";
	}
	else {
		dart::ZoneTextBuffer buffer(zone);
		const auto& result_type = dart::AbstractType::Handle(sig.result_type());
		result_type.PrintName(dart::Object::kScrubbedName, &buffer);
		of << buffer.buffer() << " " << name;
		// function type paramaters
		const auto& type_params = dart::TypeParameters::Handle(zone, sig.type_parameters());
		if (!type_params.IsNull()) {
			buffer.Clear();
			type_params.Print(dart::Thread::Current(), zone, false, 0, dart::Object::kScrubbedName, &buffer);
			of << "<" << buffer.buffer() << ">";
		}
		buffer.Clear();
		sig.PrintParameters(dart::Thread::Current(), zone, dart::Object::kScrubbedName, &buffer);
		of << "(" << buffer.buffer() << ")";
	}

	if (is_async) {
		of << " async";
	}
	of << " {\n";

	of << std::format("    // ** addr: {:#x}, size: {:#x}\n", ep_addr, size);
}

void DartFunction::PrintFoot(std::ostream& of) const
{
	of << "  }\n";
}

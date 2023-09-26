#include "pch.h"
#include "VarValue.h"
#include <sstream>

static_assert(sizeof(bool) == 1, "bool size is not 1 byte");

std::string VarStorage::Name()
{
	switch (kind) {
	case Register:
		return GetRegisterName(reg);
	case Local:
		return std::format("local_{:x}", -offset);
	case Argument:
		return std::format("arg_{}", idx);
	case Static:
		return std::format("static_{:x}", offset);
	case Pool:
		return std::format("PP_{:x}", offset);
	case Thread:
		return std::format("THR_{:x}", offset);
	case SmallImm:
		return std::to_string(offset);
	case InInstruction:
		return "tmp";
	default:
		// Immediate has no storage type
		FATAL("Unknown storage type");
	}
}

void VarValue::SetIntType(ValueType tid)
{
	// only 2 possibles containers are VarExpression and VarInteger
	if (RawTypeId() == VarValue::Expression) {
		reinterpret_cast<VarExpression*>(this)->SetType(tid);
	}
	else {
		// should be only VarInteger
		ASSERT(RawTypeId() == dart::kIntegerCid);
		reinterpret_cast<VarInteger*>(this)->intTypeId = tid;
	}
}

void VarValue::SetSmiIfInt()
{
	if (RawTypeId() == VarValue::Expression) {
		reinterpret_cast<VarExpression*>(this)->SetType(dart::kSmiCid);
	}
	else if (RawTypeId() == dart::kIntegerCid) {
		// Note: should not convert from Mint
		reinterpret_cast<VarInteger*>(this)->intTypeId = dart::kSmiCid;
	}
}

std::string VarArray::ToString()
{
	std::string out;
	if ((intptr_t)ptr == (intptr_t)dart::Object::null()) {
		// no data
		out = "List";
		if (eleType) {
			out += "<" + eleType->ToString() + ">";
		}
		out += "(";
		if (length != -1) {
			out += std::to_string(length);
		}
		out += ")";
	}
	else {
		// has data (const array)
		const auto& arr = dart::Array::Handle(ptr);
		const auto arr_len = arr.Length();
		//const auto& type_args = dart::TypeArguments::Handle(arr.GetTypeArguments());
		std::ostringstream ss;
		ss << "const [";
		if (arr_len > 0) {
			// in ImmutableList, only Dart type (native type is not used)
			const auto heap_base = dart::Thread::Current()->heap_base();
			auto& obj = dart::Object::Handle();
			auto arrPtr = dart::Array::DataOf(arr.ptr());
			for (intptr_t i = 0; i < arr_len; i++) {
				if (i != 0)
					ss << ", ";

				if (arrPtr->IsHeapObject()) {
					obj = arrPtr->Decompress(heap_base);
					// TODO: better string representation
					ss << obj.ToCString();
				}
				else {
					obj = arrPtr->DecompressSmi();
					ss << std::hex << std::showbase << dart::Smi::Cast(obj).Value();
				}
				arrPtr++;
			}
		}
		ss << "]";
		out = ss.str();
	}
	return out;
}

std::string VarItem::Name()
{
	switch (storage.kind) {
	case VarStorage::Immediate:
	case VarStorage::Pool:
		return val->ToString();
	default:
		return storage.Name();
	}
}

std::string VarItem::CallArgName()
{
	switch (storage.kind) {
	case VarStorage::Immediate:
	case VarStorage::Pool:
	case VarStorage::Register:
		return val->ToString();
	default:
		return storage.Name();
	}
}

#include "pch.h"
#include "FridaWriter.h"
#include <fstream>
#include <filesystem>
#include "Util.h"
#include "DartDumper.h"

#ifndef FRIDA_TEMPLATE_DIR
#define FRIDA_TEMPLATE_DIR "scripts"
#endif

void FridaWriter::Create(const char* filename)
{
	std::filesystem::copy_file(FRIDA_TEMPLATE_DIR "/frida.template.js", filename, std::filesystem::copy_options::overwrite_existing);

	std::ofstream of(filename, std::ios_base::app);

	of << "const ClassIdTagPos = " << kUntaggedObjectClassIdTagPos << ";\n";
	of << std::format("const ClassIdTagMask = {:#x};\n", (1 << dart::UntaggedObject::kClassIdTagSize) - 1);

	of << "const NumPredefinedCids = " << dart::kNumPredefinedCids << ";\n";
	of << "const CidObject = " << dart::kInstanceCid << ";\n";
	of << "const CidNull = " << dart::kNullCid << ";\n";
	of << "const CidSmi = " << dart::kSmiCid << ";\n";
	of << "const CidMint = " << dart::kMintCid << ";\n";
	of << "const CidDouble = " << dart::kDoubleCid << ";\n";
	of << "const CidBool = " << dart::kBoolCid << ";\n";
	of << "const CidString = " << dart::kOneByteStringCid << ";\n";
	of << "const CidArray = " << dart::kArrayCid << ";\n";
	of << "const CidGrowableArray = " << dart::kGrowableObjectArrayCid << ";\n";
	of << "const CidSet = " << dart::kSetCid << ";\n";
	of << "const CidMap = " << dart::kMapCid << ";\n";
	of << "const CidClosure = " << dart::kClosureCid << ";\n";
	of << "const CidUint8Array = " << dart::kTypedDataUint8ArrayCid << ";\n";
	of << "const CidInt8Array = " << dart::kTypedDataInt8ArrayCid << ";\n";
	of << "const CidUint16Array = " << dart::kTypedDataUint16ArrayCid << ";\n";
	of << "const CidInt16Array = " << dart::kTypedDataInt16ArrayCid << ";\n";
	of << "const CidUint32Array = " << dart::kTypedDataUint32ArrayCid << ";\n";
	of << "const CidInt32Array = " << dart::kTypedDataInt32ArrayCid << ";\n";
	of << "const CidUint64Array = " << dart::kTypedDataUint64ArrayCid << ";\n";
	of << "const CidInt64Array = " << dart::kTypedDataInt64ArrayCid << ";\n";

	of << "const Classes = [\n";
	for (auto dartCls : app.classes) {
		if (!dartCls) {
			of << "null,\n";
			continue;
		}

		if (dartCls->Id() < dart::kNumPredefinedCids) {
			switch (dartCls->Id()) {
			case dart::kBoolCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"bool\",";
				// the bool in Dart use only 2 Immutable objects (true and false)
				//of << "tptr:" << (uint64_t)dart::Bool::True().ptr() - app.heap_base() << ",";
				//of << "fptr:" << (uint64_t)dart::Bool::False().ptr() - app.heap_base() << ",";
				// value_ offset in raw_object.h is inaccessible
				of << "valOffset:" << AOT_Instance_InstanceSize << "},\n";
				break;
			case dart::kMintCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"int\",";
				of << "valOffset:" << AOT_Mint_value_offset << "},\n";
				break;
			case dart::kDoubleCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"double\",";
				of << "valOffset:" << AOT_Double_value_offset << "},\n";
				break;
			case dart::kOneByteStringCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"String\",";
				of << "lenOffset:" << AOT_String_length_offset << ",";
				of << "dataOffset:" << AOT_OneByteString_data_offset << "},\n";
				break;
			case dart::kArrayCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"List\",";
				//dart::Array::kBytesPerElement is same as a compressed pointer size
				of << "lenOffset:" << AOT_Array_length_offset << ",";
				of << "dataOffset:" << AOT_Array_data_offset << ",";
				of << "typeOffset:" << AOT_Array_type_arguments_offset << "},\n";
				break;
			case dart::kGrowableObjectArrayCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"GrowableList\",";
				of << "lenOffset:" << AOT_GrowableObjectArray_length_offset << ",";
				of << "dataOffset:" << AOT_GrowableObjectArray_data_offset << ",";
				of << "typeOffset:" << AOT_GrowableObjectArray_type_arguments_offset << "},\n";
				break;
			case dart::kSetCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"Set\",";
				of << "usedOffset:" << AOT_LinkedHashBase_used_data_offset << ",";
				of << "delOffset:" << AOT_LinkedHashBase_deleted_keys_offset << ",";
				of << "dataOffset:" << AOT_LinkedHashBase_data_offset << ",";
				of << "typeOffset:" << AOT_LinkedHashBase_type_arguments_offset << "},\n";
				break;
			case dart::kMapCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"Map\",";
				of << "usedOffset:" << AOT_LinkedHashBase_used_data_offset << ",";
				of << "delOffset:" << AOT_LinkedHashBase_deleted_keys_offset << ",";
				of << "dataOffset:" << AOT_LinkedHashBase_data_offset << ",";
				of << "typeOffset:" << AOT_LinkedHashBase_type_arguments_offset << "},\n";
				break;
			case dart::kClosureCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"Closure\",";
				of << "fnOffset:" << AOT_Closure_function_offset << ",";
				of << "contextOffset:" << AOT_Closure_context_offset << ",";
				of << "epOffset:" << AOT_Closure_entry_point_offset << "},\n";
				break;
			case dart::kTypedDataUint8ArrayCid:
			case dart::kTypedDataUint16ArrayCid:
			case dart::kTypedDataUint32ArrayCid:
			case dart::kTypedDataUint64ArrayCid:
			case dart::kTypedDataInt8ArrayCid:
			case dart::kTypedDataInt16ArrayCid:
			case dart::kTypedDataInt32ArrayCid:
			case dart::kTypedDataInt64ArrayCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:" << Util::Quote(dartCls->Name()) << ",";
				of << "lenOffset:" << AOT_TypedDataBase_length_offset << ",";
				// current version name is "AOT_TypedData_payload_offset" but old version name is "AOT_TypedData_data_offset"
				// function from UntaggedTypedData is always same
				of << "dataOffset:" << dart::UntaggedTypedData::payload_offset() << "},\n";
				break;
			case dart::kInstanceCid:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:\"Object\",";
				of << "size:" << AOT_Instance_InstanceSize << "},\n";
				break;
			default:
				of << "{id:" << dartCls->Id() << ",";
				of << "name:" << Util::Quote(dartCls->Name()) << "},\n";
				break;
			}
		}
		else {
			of << "{";
			of << "id:" << dartCls->Id() << ",";
			of << "name:" << Util::Quote(dartCls->Name()) << ",";
			of << "fbitmap:" << dartCls->FieldBitmap() << ",";
			of << "sid:" << dartCls->Parent()->Id() << ",";
			of << "size:" << dartCls->Size() << ",";
			of << "argOffset:" << dartCls->TypeArgumentOffset();// << ",";
			of << "},\n";
		}
	}
	of << "];\n";
	of << "function GetFuncPtrMap(){\nreturn new Map([\n";
	for (auto lib : app.libs) {
		std::string lib_prefix = lib->GetName();
		for (auto cls : lib->classes) {
			std::string cls_prefix = cls->Name();
			for (auto dartFn : cls->Functions()) {
				const auto ep = dartFn->Address();
				auto name = DartDumper::getFunctionName4Ida(*dartFn, cls_prefix);
				of << "[" << std::format("{:#x}, \"{}_{}::{}_{:x}\"", ep, lib_prefix, cls_prefix, name.c_str(), ep) << "],\n";
			}
		}
	}
	of << "]);\n}\n";

}

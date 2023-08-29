#include "pch.h"
#include "DartApp.h"
#include "ElfHelper.h"
#include "DartLoader.h"
PRAGMA_WARNING(push, 0)
#include <vm/stub_code.h>
#include <vm/heap/safepoint.h>
PRAGMA_WARNING(pop)
#include <format>
#include <iostream> // for debugging purpose

DartApp::DartApp(const char* path) : ppool(NULL), nativeLib(0xdeadead), throwStubAddr(0)
{
	auto libInfo = ElfHelper::MapLibAppSo(path);
	lib_base = libInfo.lib;
	vm_snapshot_data = libInfo.vm_snapshot_data;
	vm_snapshot_instructions = libInfo.vm_snapshot_instructions;
	isolate_snapshot_data = libInfo.isolate_snapshot_data;
	isolate_snapshot_instructions = libInfo.isolate_snapshot_instructions;

	isolate = reinterpret_cast<dart::Isolate*>(DartLoader::Load(libInfo));

	heap_base_ = dart::Thread::Current()->heap_base();
	inScope = false;

	DartFnBase::SetLibBase(base());

	dartIntCid = 0;
	dartDoubleCid = 0;
	dartStringCid = 0;
	dartBoolCid = 0;
	dartRecordCid = 0;
	dartListCid = 0;
	dartSetCid = 0;
	dartMapCid = 0;
	dartRunesCid = 0;
}

DartApp::~DartApp()
{
	ExitScope();
	DartLoader::Unload();

	for (auto lib : libs) {
		delete lib;
	}
	// classes are just reference. owners are in libraries. DO NOT delete here

	for (auto& stub : stubs) {
		delete stub.second;
	}
}

void DartApp::EnterScope()
{
	if (!inScope) {
		inScope = true;
		Dart_EnterScope();
		// exit safepoint so new symbols can be created
		//dart::Thread::Current()->SetAtSafepoint(false);
		isolate->safepoint_handler()->ExitSafepointUsingLock(dart::Thread::Current());
		
		ppool = reinterpret_cast<dart::ObjectPool*>(dart::VMHandles::AllocateHandle(dart::Thread::Current()->zone()));
		*ppool = isolate->group()->object_store()->global_object_pool();
	}
}

void DartApp::ExitScope()
{
	if (inScope) {
		inScope = false;
		Dart_ExitScope();
		ppool = NULL;
	}
}

DartClass* DartApp::GetClass(intptr_t cid)
{
	if ((size_t)cid > classes.size()) {
		// assume top level class
		return topClasses.at(dart::ClassTable::IndexFromTopLevelCid(cid));
	}
	return classes.at(cid);
}

DartFnBase* DartApp::GetFunction(uint64_t addr)
{
	auto fn = functions.find(addr);
	if (fn != functions.end()) {
		return fn->second;
	}

	auto stub = stubs.find(addr);
	if (stub != stubs.end()) {
		return stub->second;
	}
	// another possible is duplicated stubs in one big stub
	for (auto& [ep_addr, stub] : stubs) {
		if (stub->Address() < addr && addr < stub->AddressEnd()) {
			auto newStub = stub->Split(addr);
			stubs[addr] = newStub;
			return newStub;
		}
	}
	return nullptr;
}

DartLibrary* DartApp::addLibraryClass(const dart::Library& library, const dart::Class& cls)
{
	const auto topCid = library.toplevel_class().untag()->id();
	const auto idx = dart::ClassTable::IndexFromTopLevelCid(topCid);
	auto dartCls = topClasses[idx];
	DartLibrary* dartLib;
	if (dartCls == NULL) {
		dartLib = addLibrary(library);
	}
	else {
		dartLib = const_cast<DartLibrary*>(&dartCls->lib);
	}

	// Note: this function is called only when the cls is missing
	// library always contains top level class and it is automatically add when adding Library
	if (!cls.IsTopLevel()) {
		auto dartCls = dartLib->AddClass(cls);

		classes[dartCls->id] = dartCls;
		for (const auto dartFn : dartCls->functions) {
			functions[dartFn->Address()] = dartFn;
		}
	}
	return dartLib;
}

DartLibrary* DartApp::addLibrary(const dart::Library& library)
{
	auto lib = new DartLibrary(library);
	libs.push_back(lib);

	// add classes and functions for mapping from address
	for (const auto dartCls : lib->classes) {
		if (dartCls == lib->topClass)
			topClasses[dart::ClassTable::IndexFromTopLevelCid(dartCls->id)] = dartCls;
		else
			classes[dartCls->id] = dartCls;
		for (const auto dartFn : dartCls->functions) {
			functions[dartFn->Address()] = dartFn;
		}
	}

	return lib;
}

void DartApp::LoadInfo()
{
	auto ig = dart::Isolate::Current()->group();

	loadFromClassTable(ig->class_table());

	auto store = ig->object_store();

	// load pre-defined stub
	loadStubs(store);

	loadFromInstructions(store);

	finalizeFunctionsInfo();

	//auto fieldTable = isolate->field_table(); //contains only sentinel, null, false, 0

	// there are instruction tables in vm isolate but their code are not called from Dart code (can be skipped)
	//dart::Dart::vm_isolate_group();

	loadFromObjectPool();
}

void DartApp::loadFromClassTable(dart::ClassTable* table)
{
	const auto num_cids = table->NumCids();
	const auto num_top_cids = table->NumTopLevelCids();
	classes.resize(num_cids);
	topClasses.resize(num_top_cids);
	libs.reserve(num_top_cids);

	auto& library = dart::Library::Handle();
	auto& cls = dart::Class::Handle();

	// https://github.com/dart-lang/sdk/issues/52310
	// New version of Dart, information for classes in libraries and methods in classes are missing.
	//   But there are information about the methods owner and classes owner.
	//   So, we can work backward to fill the libraries and classes information

	// iterate from toplevel class table to genreate libraries
	for (intptr_t i = 0; i < num_top_cids; i++) {
		const auto topCid = dart::ClassTable::CidFromTopLevelIndex(i);
		auto clsPtr = table->At(topCid);
		if (clsPtr == nullptr)
			continue;

		cls = clsPtr;
		library = cls.library();
		addLibrary(library);
	}

	// load from class table
	for (intptr_t i = 0; i < num_cids; i++) {
		auto clsPtr = table->At(i);
		if (clsPtr == nullptr) {
			ASSERT(!classes[i]);
			continue;
		}

		if (classes[i] == nullptr) {
			cls = clsPtr;
			library = cls.library();
			if (library.IsNull()) {
				auto dartCls = nativeLib.AddClass(cls);
				classes[i] = dartCls;
			}
			else {
				addLibraryClass(library, cls);
			}
		}

		classes[i]->unboxed_fields_bitmap = table->GetUnboxedFieldsMapAt(i);
	}

	// post process of classes
	// map super class and native type class ids
	for (auto dartCls : classes) {
		if (dartCls == NULL)
			continue;

		if (dartCls->superCls)
			dartCls->superCls = classes[(intptr_t)dartCls->superCls];

		// Dart create a new class for int, double, ... (do not know why built-in is not used)
		if (dartCls->name == "int") dartIntCid = dartCls->id;
		else if (dartCls->name == "double") dartDoubleCid = dartCls->id;
		else if (dartCls->name == "String") dartStringCid = dartCls->id;
		else if (dartCls->name == "bool") dartBoolCid = dartCls->id;
		else if (dartCls->name == "Record") dartRecordCid = dartCls->id;
		else if (dartCls->name == "List") dartListCid = dartCls->id;
		else if (dartCls->name == "Set") dartSetCid = dartCls->id;
		else if (dartCls->name == "Map") dartMapCid = dartCls->id;
		else if (dartCls->name == "Runes") dartRunesCid = dartCls->id;
	}

	typeDb = std::unique_ptr<DartTypeDb>(new DartTypeDb(classes));

	// complete the class info after super class is set
	auto zone = dart::Thread::Current()->zone();
	auto& interfaces = dart::Array::Handle(zone);
	auto& type = dart::Type::Handle(zone);
	for (auto dartCls : classes) {
		if (dartCls == nullptr || dartCls->superCls == nullptr)
			continue;

		cls = dartCls->ptr;
		auto dartType = typeDb->FindOrAdd(cls.DeclarationType());
		dartCls->declarationType = dartType;
		ASSERT(dartType->AsType()->Class().Id() == dartCls->Id());
		// Note: below subvector type might be wrong for complicated generic type
		if (dartCls->num_type_parameters > 0) {
			dartCls->typeVectorName = dartType->Arguments().SubvectorName(0, dartCls->num_type_parameters);
		}
		if (dartCls->superCls->num_type_parameters > 0) {
			dartCls->parentTypeVectorName = dartType->Arguments().SubvectorName(0, dartCls->superCls->num_type_parameters);
		}

		// if there is a mixin, the last one is mixin
		// TODO: correct multiple interfaces or mixins because compiler generate dummy classes for "extends" and "with" 1 class
		interfaces = cls.interfaces();
		auto interfaces_len = interfaces.Length();
		if (dartCls->is_transformed_mixin) {
			type ^= interfaces.At(--interfaces_len);
			dartCls->mixin = classes[type.type_class_id()];
			ASSERT(dartCls->mixin);
		}
		for (auto i = 0; i < interfaces_len; i++) {
			type ^= interfaces.At(i);
			dartCls->interfaces.push_back(classes[type.type_class_id()]);
		}
	}
}

void DartApp::loadStubs(dart::ObjectStore* store)
{
	dart::CodePtr ptr;
	auto& code = dart::Code::Handle();
	uint64_t ep_addr;
	DartStub* stub;

	// Note: some stub might contain multiple of duplicated stubs
	// these stubs are called "_iso_stub_" in runtime/vm/stub_code.cc
#define DO(member, name) \
	ptr = store->member(); \
	code = ptr; \
	ep_addr = code.EntryPoint() - base(); \
	stub = new DartStub(ptr, DartStub::name ## Stub, ep_addr, code.Size(), #name); \
	ASSERT(!stubs.contains(ep_addr)); \
	stubs[ep_addr] = stub;
	OBJECT_STORE_STUB_CODE_LIST(DO);
	DO(build_nongeneric_method_extractor_code, BuildNonGenericMethodExtractor);
	DO(build_generic_method_extractor_code, BuildGenericMethodExtractor);
#undef DO
	
	code = store->throw_stub();
	throwStubAddr = code.EntryPoint();

	// load VM stub code
	// the dart entry point "static void main()" is a LazyCompileVMStub which call "main" stub (a real main)
	ASSERT(dart::StubCode::HasBeenInitialized());
#define DO(name) {\
		const auto& code = dart::StubCode::name(); \
		ep_addr = code.EntryPoint() - base(); \
		if (stubs.contains(ep_addr)) { \
			ASSERT(stubs[ep_addr]->Name() == #name); \
		} \
		else { \
			stub = new DartStub(code.ptr(), DartStub::name ## VMStub, ep_addr, code.Size(), #name); \
			stubs[ep_addr] = stub; \
			auto it = functions.find(ep_addr); \
			if (it != functions.end()) { \
				auto dartFn = it->second; \
				std::erase(dartFn->Class().functions, dartFn); \
				functions.erase(it); \
			} \
		} \
	}
	VM_STUB_CODE_LIST(DO);
#undef DO
}

void DartApp::loadFromInstructions(dart::ObjectStore* store)
{
	auto zone = dart::Thread::Current()->zone();
	auto& code = dart::Code::Handle(zone);
	auto& obj = dart::Object::Handle(zone);

	// find other functions from instruction tables
	auto tables = store->instructions_tables().untag();
	const auto tables_length = dart::Smi::Value(tables->length());
	for (intptr_t i = 0; i < tables_length; i++) {
		const auto table = static_cast<dart::InstructionsTablePtr>(tables->data().untag()->element(i));
		const auto code_objects = table.untag()->code_objects().untag();
		const auto code_objects_length = dart::Smi::Value(code_objects->length());
		for (intptr_t j = 0; j < code_objects_length; j++) {
			const auto obj_ptr = code_objects->element(j);
			const auto code_ptr = dart::Code::RawCast(obj_ptr);

			code = code_ptr;
			const auto entry_point = code.EntryPoint();
			const auto ep_offset = entry_point - base();

			auto owner = code.owner();
			if ((intptr_t)owner == (intptr_t)dart::Object::null()) {
				assert(code.IsStubCode());
				if (!stubs.contains(ep_offset)) {
					//std::cout << std::format("unknown stub at: {:#x}, {}, size: {}\n", ep_offset, code.ToCString(), code.Size());
					auto stub_size = code.Size();
					DartStub* candidateStub = nullptr;
					std::vector<DartStub*> candidateStubs;
					for (auto const& [stubEp, stub] : stubs) {
						if (stub->kind < DartStub::SharedStub && stub->Size() == (int64_t)stub_size) {
							if (memcmp((void*)stub->MemAddress(), (void*)entry_point, stub_size) == 0) {
								// exact match
								candidateStub = stub;
								break;
							}
							candidateStubs.push_back(stub);
						}
					}
					if (candidateStub == nullptr) {
						RELEASE_ASSERT(!candidateStubs.empty());
						if (candidateStubs.size() == 1) {
							candidateStub = candidateStubs[0];
						}
						else {
							uint32_t maxMatch = 0;
							for (auto stub : candidateStubs) {
								auto tmpCode = (uint8_t*)stub->MemAddress();
								uint32_t cnt = 0;
								for (size_t i = 0; i < stub_size; i++) {
									if (((uint8_t*)entry_point)[i] == tmpCode[i])
										cnt++;
								}
								if (cnt > maxMatch) {
									candidateStub = stub;
									maxMatch = cnt;
								}
							}
						}
					}
					//std::cout << std::format("unknown stub at: {:#x}, map to {}\n", ep_offset, candidateStub->Name());
					ASSERT(candidateStub);
					stubs[ep_offset] = new DartStub(code_ptr, candidateStub->kind, ep_offset, stub_size, candidateStub->Name());
				}
				continue;
			}

			obj = owner;
			if (obj.IsClass()) {
				// stub of user class
				if (stubs.contains(ep_offset)) {
					throw std::runtime_error("not allocate stub for user class");
				}
				const auto cid = (uint32_t)dart::Class::Cast(obj).id();
				auto astub = new DartAllocateStub(code_ptr, ep_offset, code.Size(), cid, classes[cid]->name);
				stubs[ep_offset] = astub;
			}
			else if (obj.IsAbstractType()) {
				// Type test stub (seen use case: cast with "as" - xx as String)
				// this MUST be kTypeCid
				if (!obj.IsType())
					throw std::runtime_error("TestType is not Type");
				if (stubs.contains(ep_offset))
					throw std::runtime_error("duplitcate stub entry point");
				auto dartType = typeDb->FindOrAdd(dart::Type::Cast(obj).ptr());
				auto tstub = new DartTypeStub(code_ptr, ep_offset, code.Size(), dart::Type::Cast(obj).ptr(), dartType->ToString());
				stubs[ep_offset] = tstub;
			}
			else if (obj.IsFunction()) {
				if (!code.is_optimized())
					throw std::runtime_error("function code is not optimized");
				// functions might not be in from Libraries
				// they might be closure, indirect call, ... (know only closure usage)
				if (!functions.contains(ep_offset)) {
					// find its class or library, then add it
					const auto& func = dart::Function::Cast(obj);
					const auto cls_ptr = func.Owner();
					const auto cid = cls_ptr.untag()->id();
					DartClass* cls;
					if (dart::ClassTable::IsTopLevelCid(cid)) {
						const auto idx = dart::ClassTable::IndexFromTopLevelCid(cid);
						cls = topClasses[idx];
						if (cls == NULL) {
							// new library
							const auto& clsHandle = dart::Class::Handle(cls_ptr);
							const auto& library = dart::Library::Handle(clsHandle.library());
							cls = addLibrary(library)->topClass;
						}
					}
					else {
						cls = classes[cid];
						if (cls == NULL) {
							auto msg = std::format("found invalid class id: {}", cid);
							throw std::runtime_error(msg);
						}
					}
					auto dartFn = cls->AddFunction(func.ptr());
					//std::cout << std::format("missing function at: {:#x}, {}\n", ep_offset, dartFn->fullName());
					functions[dartFn->Address()] = dartFn;
				}
			}
			else {
				auto msg = std::format("unknown code at: {:#x}, {}\n", ep_offset, obj.ToCString());
				throw std::runtime_error(msg);
			}
		}
	}
}

void DartApp::finalizeFunctionsInfo()
{
	for (auto& [ep_addr, fnBase] : functions) {
		if (fnBase->IsStub())
			continue;

		auto dartFn = fnBase->AsFunction();
		// update parent pointer
		if (dartFn->parent) {
			const auto ep_addr = (intptr_t)dartFn->parent;
			if (stubs.contains(ep_addr)) {
				dartFn->parent = nullptr;
			}
			else {
				dartFn->parent = functions.at((intptr_t)dartFn->parent);
			}
		}

		// TODO: handle function result type and paramters type
	}
}

void DartApp::walkObject(dart::Object& obj)
{
	auto cid = obj.GetClassId();
	if (cid < dart::kNumPredefinedCids) {
		// objects in array, map, set
		if (obj.IsArray()) {
			const auto& arr = dart::Array::Cast(obj);
			const auto arr_len = arr.Length();
			if (arr_len > 0) {
				auto arrPtr = dart::Array::DataOf(arr.ptr());
				for (intptr_t i = 0; i < arr_len; i++) {
					if (arrPtr->IsHeapObject()) {
						obj = arrPtr->Decompress(heap_base());
						walkObject(obj);
					}
					arrPtr++;
				}
			}
		}
		else if (obj.IsMap()) {
			auto& map = dart::Map::Cast(obj);
			dart::Map::Iterator iter(map);
			auto& obj2 = dart::Object::Handle();
			while (iter.MoveNext()) {
				obj2 = iter.CurrentKey();
				walkObject(obj2);
				obj2 = iter.CurrentValue();
				walkObject(obj2);
			}
		}
		else if (obj.IsSet()) {
			auto& set = dart::Set::Cast(obj);
			dart::Set::Iterator iter(set);
			auto& obj2 = dart::Object::Handle();
			while (iter.MoveNext()) {
				obj2 = iter.CurrentKey();
				walkObject(obj2);
			}
		}
		else if (obj.IsTypeArguments()) {
			typeDb->FindOrAdd(dart::TypeArguments::RawCast(obj.ptr()));
		}
		else if (obj.IsType()) {
			typeDb->FindOrAdd(dart::Type::RawCast(obj.ptr()));
		}
		else if (obj.IsTypeParameter()) {
			typeDb->FindOrAdd(dart::TypeParameter::RawCast(obj.ptr()));
		}
		else if (obj.IsFunctionType()) {
			typeDb->FindOrAdd(dart::FunctionType::RawCast(obj.ptr()));
		}
		return;
	}

	ASSERT(obj.IsInstance());

	auto dartCls = classes[cid];
	ASSERT(dartCls);
	typeDb->FindOrAdd(*dartCls, dart::Instance::Cast(obj));

	const auto bitmap = dartCls->unboxed_fields_bitmap;
	auto offset = dart::Instance::NextFieldOffset();
	const auto ptr = dart::UntaggedObject::ToAddr(obj.ptr());
	// from InstanceDeserializationCluster::ReadFill() in app_snapshot.cc
	while (offset < dartCls->size) {
		if (bitmap.Get(offset / dart::kCompressedWordSize)) {
			// AOT uses native integer if it is less than 31 bits (compressed pointer)
			// integer (4/8 bytes) or double (8 bytes)
			if (dart::kCompressedWordSize == 4) {
				RELEASE_ASSERT(bitmap.Get((offset + dart::kCompressedWordSize) / dart::kCompressedWordSize));
			}
			auto p = reinterpret_cast<uint64_t*>(ptr + offset);
			// it is rare to find integer that larger than 0x1000_0000_0000_0000
			//   while double is very common because of exponent value
			// to know exact type (int or double), we have to check from register type in assembly
			if (*p <= 0x1000000000000000 || *p >= 0xffffffffffff0000) {
				dartCls->AddField(offset, typeDb->Get(dart::kMintCid));
			}
			else {
				dartCls->AddField(offset, typeDb->Get(dart::kDoubleCid));
			}
			//std::cout << std::format("    offset_{:x} : int({:#x})\n", offset, *p);
			offset += dart::kCompressedWordSize * 2;
		}
		else {
			auto p = reinterpret_cast<dart::CompressedObjectPtr*>(ptr + offset);
			if (!p->IsHeapObject()) {
				// SMI. assume Mint but the value is small
				dartCls->AddField(offset, typeDb->Get(dart::kMintCid));
			}
			else {
				const auto objPtr2 = p->Decompress(heap_base());
				if (objPtr2.GetClassId() != dart::kNullCid) {
					if (offset == dartCls->TypeArgumentsOffset()) {
						ASSERT(objPtr2.GetClassId() == dart::kTypeArgumentsCid);
						typeDb->FindOrAdd(dart::TypeArguments::RawCast(objPtr2));
					}
					else {
						// compressed object ptr
						const auto fieldCid = objPtr2.GetClassId();
						const auto fieldCls = classes[fieldCid];
						if (fieldCls) {
							obj = objPtr2;
							dartCls->AddField(offset, typeDb->FindOrAdd(*fieldCls, dart::Instance::Cast(obj)));
							// walk this object recursively
							walkObject(obj);
						}
						else {
							//dart::kCallSiteDataCid;
						}
					}
				}
			}
			offset += dart::kCompressedWordSize;
		}
	}

}

void DartApp::loadFromObjectPool()
{
	const auto& pool = GetObjectPool();
	intptr_t num = pool.Length();

	auto& obj = dart::Object::Handle();

	for (intptr_t i = 0; i < num; i++) {
		const auto objType = pool.TypeAt(i);
		if (objType == dart::ObjectPool::EntryType::kTaggedObject) {
			obj = pool.ObjectAt(i);
			if (obj.IsField()) {
				const auto& field = dart::Field::Cast(obj);
				auto dartCls = GetClass(field.Owner().untag()->id());
				auto dartField = dartCls->AddField(field.ptr());
				if (dartField->IsStatic()) {
					ASSERT(!staticFields.contains(dartField->Offset()));
					staticFields[dartField->Offset()] = dartField;
				}
			}
			walkObject(obj);
		}
		else if (objType == dart::ObjectPool::EntryType::kImmediate) {
			// just immediate. no info
		}
		else if (objType == dart::ObjectPool::EntryType::kNativeFunction) {
			// normally, it is only used in internal library (can be ignored)
		}
		else {
			throw std::runtime_error("Unknown Object Pool entry type");
		}
	}
}

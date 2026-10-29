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

bool DartApp::recover_anonymous_code_ = true;

DartApp::DartApp(const char* path) : ppool(NULL), nativeLib(0xdeadead), throwStubAddr(0)
{
	auto libInfo = ElfHelper::MapLibAppSo(path);
	lib_base = libInfo.lib;
	vm_snapshot_data = libInfo.vm_snapshot_data;
	vm_snapshot_instructions = libInfo.vm_snapshot_instructions;
	isolate_snapshot_data = libInfo.isolate_snapshot_data;
	isolate_snapshot_instructions = libInfo.isolate_snapshot_instructions;
	text_start_ = libInfo.text_start;
	text_end_ = libInfo.text_end;

	isolate = reinterpret_cast<dart::Isolate*>(DartLoader::Load(libInfo));

	heap_base_ = dart::Thread::Current()->heap_base();
	inScope = false;

	DartFnBase::SetLibBase(base());
	DartFnBase::SetLibSize((intptr_t)libInfo.file_size);

	dartIntCid = 0;
	dartDoubleCid = 0;
	dartStringCid = 0;
	dartBoolCid = 0;
	dartRecordCid = 0;
	dartListCid = 0;
	dartSetCid = 0;
	dartMapCid = 0;
	dartRunesCid = 0;
	dartFutureCid = 0;
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

std::vector<DartLibrary*> DartApp::DumpableLibs()
{
	std::vector<DartLibrary*> out;
	for (auto lib : libs) {
		if (!lib->isInternal)
			out.push_back(lib);
	}
	// nativeLib is a member object, never owned by libs
	if (nativeLib.topClass && !nativeLib.topClass->Functions().empty())
		out.push_back(&nativeLib);
	return out;
}

void DartApp::LoadInfo()
{
	auto ig = dart::Isolate::Current()->group();

	// functions recovered from the heap carry no library; they are real app code
	// and must be dumped as well
	nativeLib.name = "native";
	nativeLib.url = "native";
	nativeLib.isInternal = false;

	loadFromClassTable(ig);

	auto store = ig->object_store();

	// load pre-defined stub
	loadStubs(store);

	// getting hidden functions from InstructionsTable are not compatible against old Dart version
	// find all Code object in heap is a work around for getting all functions
	findFunctionInHeap();

	loadFromObjectPool();

	finalizeFunctionsInfo();

	fixupFunctionBounds();

	if (recover_anonymous_code_)
		recoverAnonymousCode();

	size_t total_fn = 0, with_body = 0;
	for (auto lib : DumpableLibs()) {
		for (auto cls : lib->classes) {
			for (auto fn : cls->Functions()) {
				total_fn++;
				if (fn->Size() > 0)
					with_body++;
			}
		}
	}
	fprintf(stderr, "[blutter] dumpable libraries=%zu functions=%zu with-body=%zu\n",
	        DumpableLibs().size(), total_fn, with_body);
	fflush(stderr);

	//auto fieldTable = isolate->field_table(); //contains only sentinel, null, false, 0

	// there are instruction tables in vm isolate but their code are not called from Dart code (can be skipped)
	//dart::Dart::vm_isolate_group();
}

void DartApp::loadFromClassTable(dart::IsolateGroup* ig)
{
	auto table = ig->class_table();
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

#ifdef HAS_SHARED_CLASS_TABLE
		classes[i]->unboxed_fields_bitmap = ig->shared_class_table()->GetUnboxedFieldsMapAt(i);
#else
		classes[i]->unboxed_fields_bitmap = table->GetUnboxedFieldsMapAt(i);
#endif
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
		else if (dartCls->name == "Future") dartFutureCid = dartCls->id;
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
#ifndef NO_METHOD_EXTRACTOR_STUB
	DO(build_nongeneric_method_extractor_code, BuildNonGenericMethodExtractor);
	DO(build_generic_method_extractor_code, BuildGenericMethodExtractor);
#endif
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

DartFunction* DartApp::addFunctionNoCheck(const dart::Function& func)
{
	// find its class or library, then add it
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
	return cls->AddFunction(func.ptr());
}

void DartApp::addFunction(uintptr_t ep_addr, const dart::Function& func)
{
	if (!functions.contains(ep_addr)) {
		auto dartFn = addFunctionNoCheck(func);
		functions[dartFn->Address()] = dartFn;
	}
}

class HeapCodeVisitor : public dart::ObjectVisitor {
public:
	explicit HeapCodeVisitor(std::vector<dart::CodePtr>& codePtrs) : codePtrs(codePtrs) {}
	virtual ~HeapCodeVisitor() {}

	// Invoked for each object.
	virtual void VisitObject(dart::ObjectPtr obj) {
		if (obj->IsCode())
			codePtrs.push_back(dart::Code::RawCast(obj));
	}

private:
	std::vector<dart::CodePtr>& codePtrs;
};

void DartApp::fixupFunctionBounds()
{
	// AOT code is laid out back to back, so a function cannot extend past the next
	// entry point. Two problems show up in hardened builds: Function.code_ is
	// blanked out (size unknown) and some Code objects claim a range that covers
	// the following function. Fill in unknown sizes from the next entry point or
	// the next "stp fp, lr, [sp, #-N]!" prologue, whichever comes first, and cut
	// a range that runs into the next function.
	std::vector<DartFunction*> all;
	for (auto lib : libs) {
		for (auto cls : lib->classes) {
			for (auto fn : cls->Functions()) {
				if (fn->Address() > 0 &&
				    fn->Address() < (uint64_t)DartFnBase::LibSize())
					all.push_back(fn);
			}
		}
	}
	if (all.empty())
		return;

	std::sort(all.begin(), all.end(), [](DartFunction* a, DartFunction* b) {
		return a->Address() < b->Address();
	});

	constexpr int64_t kMaxGuess = 0x8000;
	const auto lib_size = (int64_t)DartFnBase::LibSize();
	const auto* image = (const uint8_t*)lib_base;
	static const uint8_t kPrologue[2][4] = {
		{ 0xfd, 0x79, 0xbf, 0xa9 },  // stp fp, lr, [sp, #-0x10]!
		{ 0xfd, 0x7b, 0xbf, 0xa9 },  // stp fp, lr, [sp, #-0x20]!
	};

	size_t changed = 0;
	for (size_t i = 0; i < all.size(); i++) {
		auto fn = all[i];
		const auto addr = (int64_t)fn->Address();
		const auto known_size = fn->Size();
		const auto next_entry = (i + 1 < all.size()) ? (int64_t)all[i + 1]->Address()
		                                            : lib_size;
		int64_t limit = known_size;
		if (known_size <= 0) {
			limit = (next_entry > addr) ? (next_entry - addr) : kMaxGuess;
			if (limit > kMaxGuess)
				limit = kMaxGuess;
		}
		if (limit > kMaxGuess)
			limit = kMaxGuess;
		if (addr + limit > lib_size)
			limit = lib_size - addr;
		if (limit <= 0)
			continue;

		int64_t guess = 0;
		for (int64_t off = 4; off < limit; off += 4) {
			const uint8_t* p = image + addr + off;
			if (memcmp(p, kPrologue[0], 4) == 0 ||
			    memcmp(p, kPrologue[1], 4) == 0) {
				guess = off;
				break;
			}
		}
		if (known_size <= 0) {
			if (guess <= 0)
				guess = limit;
			if (guess > 0) {
				fn->SetSize(guess);
				changed++;
			}
		}
		else if (next_entry > addr && addr + known_size > next_entry) {
			// the Code object claims a range that covers the following function;
			// well formed snapshots never get here
			const auto bounded = (guess > 0) ? guess : (next_entry - addr);
			if (bounded > 0 && bounded < known_size) {
				fn->SetSize(bounded);
				changed++;
			}
		}
	}

	fprintf(stderr, "[blutter] fixed bounds of %zu functions (%zu candidates)\n",
	        changed, all.size());
	fflush(stderr);
}

void DartApp::recoverAnonymousCode()
{
	// Hardened AOT builds can drop the Function/Code objects of app code from the
	// snapshot, leaving machine code that nothing points at. Such code is found
	// again by scanning the executable segments for function prologues that are
	// not covered by any known function.
	if (text_start_ == 0 || text_end_ <= text_start_)
		return;

	std::vector<std::pair<uint64_t, uint64_t>> known;
	std::vector<uint64_t> starts;
	auto add_fn = [&known, &starts](DartFnBase* fn) {
		const auto addr = fn->Address();
		if (addr == 0)
			return;
		starts.push_back(addr);
		const auto size = fn->Size();
		if (size > 0) {
			known.emplace_back(addr, addr + (uint64_t)size);
		}
	};
	for (auto lib : libs) {
		for (auto cls : lib->classes) {
			for (auto fn : cls->Functions())
				add_fn(fn);
		}
	}
	if (nativeLib.topClass) {
		for (auto fn : nativeLib.topClass->Functions())
			add_fn(fn);
	}
	// stubs are not used as coverage here: several stubs can be packed into one
	// Code object, so their reported range can swallow app code that follows
	// them. Code inside a stub has its own prologue and is recovered as usual.

	std::sort(known.begin(), known.end());
	std::vector<std::pair<uint64_t, uint64_t>> spans;
	for (auto& r : known) {
		if (!spans.empty() && r.first <= spans.back().second)
			spans.back().second = std::max(spans.back().second, r.second);
		else
			spans.push_back(r);
	}
	auto is_covered = [&spans](uint64_t addr) {
		auto it = std::upper_bound(spans.begin(), spans.end(), addr,
			[](uint64_t a, const std::pair<uint64_t, uint64_t>& r) { return a < r.first; });
		if (it == spans.begin())
			return false;
		--it;
		return addr >= it->first && addr < it->second;
	};

	static const uint8_t kPrologue[2][4] = {
		{ 0xfd, 0x79, 0xbf, 0xa9 },  // stp fp, lr, [sp, #-0x10]!
		{ 0xfd, 0x7b, 0xbf, 0xa9 },  // stp fp, lr, [sp, #-0x20]!
	};
	const auto* image = (const uint8_t*)lib_base;
	std::vector<uint64_t> cands;
	for (uint64_t addr = text_start_; addr + 4 <= text_end_; addr += 4) {
		const auto* p = image + addr;
		if (memcmp(p, kPrologue[0], 4) != 0 && memcmp(p, kPrologue[1], 4) != 0)
			continue;
		if (is_covered(addr))
			continue;
		cands.push_back(addr);
	}

	// Leaf functions have no frame prologue, so the scan above misses them. Their
	// callers still branch straight to the entry, so BL / B targets seed the rest.
	// Targets inside a stub are skipped: several stubs can share one Code object
	// and their reported range covers unrelated code.
	std::vector<std::pair<uint64_t, uint64_t>> stub_spans;
	for (auto& [_, stub] : stubs) {
		if (stub->Size() > 0)
			stub_spans.emplace_back(stub->Address(), stub->Address() + (uint64_t)stub->Size());
	}
	std::sort(stub_spans.begin(), stub_spans.end());
	auto in_any = [](const std::vector<std::pair<uint64_t, uint64_t>>& list, uint64_t addr) {
		auto it = std::upper_bound(list.begin(), list.end(), addr,
			[](uint64_t a, const std::pair<uint64_t, uint64_t>& r) { return a < r.first; });
		if (it == list.begin())
			return false;
		--it;
		return addr >= it->first && addr < it->second;
	};
	for (uint64_t addr = text_start_; addr + 4 <= text_end_; addr += 4) {
		const uint32_t insn = *(const uint32_t*)(image + addr);
		if ((insn & 0xfc000000) != 0x94000000 && (insn & 0xfc000000) != 0x14000000)
			continue;
		const auto rel = ((int64_t)(int32_t)(insn << 6)) >> 6;
		const auto target = (int64_t)addr + (rel << 2);
		if (target < (int64_t)text_start_ || target >= (int64_t)text_end_)
			continue;
		if (is_covered((uint64_t)target) || in_any(stub_spans, (uint64_t)target))
			continue;
		cands.push_back((uint64_t)target);
	}
	std::sort(cands.begin(), cands.end());
	cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
	if (cands.empty())
		return;

	std::vector<uint64_t> bounds = starts;
	bounds.insert(bounds.end(), cands.begin(), cands.end());
	std::sort(bounds.begin(), bounds.end());
	bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

	// one synthetic library per 64KB of code to keep the dumped files readable
	std::unordered_map<uint64_t, DartClass*> chunks;
	size_t created = 0;
	constexpr uint64_t kChunkShift = 16;
	constexpr uint64_t kMaxBody = 0x8000;
	for (auto addr : cands) {
		auto it = std::upper_bound(bounds.begin(), bounds.end(), addr);
		uint64_t end = (it != bounds.end()) ? *it : (uint64_t)text_end_;
		if (end <= addr)
			continue;
		if (end - addr > kMaxBody)
			end = addr + kMaxBody;

		const auto chunk = addr >> kChunkShift;
		auto chunkIt = chunks.find(chunk);
		if (chunkIt == chunks.end()) {
			auto lib = new DartLibrary((intptr_t)(0x5ec00000 + chunk));
			lib->name = std::format("recovered_{:x}", chunk);
			lib->url = lib->name;
			lib->isInternal = false;
			auto cls = new DartClass(*lib);
			lib->topClass = cls;
			lib->classes.push_back(cls);
			libs.push_back(lib);
			chunkIt = chunks.emplace(chunk, cls).first;
		}
		auto fn = new DartFunction(*chunkIt->second, addr, (int64_t)(end - addr),
			std::format("sub_{:x}", addr));
		chunkIt->second->Functions().push_back(fn);
		functions[addr] = fn;
		created++;
	}

	fprintf(stderr, "[blutter] recovered %zu functions from %.1f MB of unattributed code\n",
	        created, (double)(text_end_ - text_start_) / 1024.0 / 1024.0);
	fflush(stderr);
}

void DartApp::findFunctionInHeap()
{
	std::vector<dart::CodePtr> codePtrs;
	dart::HeapIterationScope heap_iteration_scope(dart::Thread::Current());
	HeapCodeVisitor visitor(codePtrs);
	heap_iteration_scope.IterateOldObjects(&visitor);

	auto zone = dart::Thread::Current()->zone();
	auto& code = dart::Code::Handle(zone);
	auto& obj = dart::Object::Handle(zone);

	for (dart::CodePtr code_ptr : codePtrs) {
		code = code_ptr;
		const auto entry_point = code.EntryPoint();
		const auto ep_offset = entry_point - base();

		auto owner = code.owner();
		if ((intptr_t)owner == (intptr_t)dart::Object::null()) {
			ASSERT(code.IsStubCode());
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
			// this MUST be kTypeCid or kRecordTypeCid (Dart >= 3.0)
			if (!obj.IsType()) {
#ifdef HAS_RECORD_TYPE
				if (!obj.IsRecordType()) {
					std::cerr << std::format("TestType is not for Type or RecordType, but for {}\n", classes[obj.GetClassId()]->Name());
				}
#else
				std::cerr << std::format("TestType is not for Type, but for {}\n", classes[obj.GetClassId()]->Name());
#endif
			}
			if (stubs.contains(ep_offset))
				throw std::runtime_error("duplitcate stub entry point");
			auto dartType = typeDb->FindOrAdd(dart::AbstractType::Cast(obj).ptr());
			auto tstub = new DartTypeStub(code_ptr, ep_offset, code.Size(), *dartType, dartType->ToString());
			stubs[ep_offset] = tstub;
		}
		else if (obj.IsFunction()) {
			ASSERT(code.is_optimized());
			// functions might not be in from Libraries
			// they might be closure, indirect call, ... (know only closure usage)
			addFunction(ep_offset, dart::Function::Cast(obj));
		}
		else if (obj.IsSmi()) {
			// this case is only seen in obfuscated app
			auto ownerClassId = code.OwnerClassId();
			ASSERT(ownerClassId == dart::kFunctionCid);
			if (!functions.contains(ep_offset)) {
				// no function, can only make it into top class of native library
				if (!nativeLib.topClass) {
					nativeLib.topClass = new DartClass(nativeLib);
					nativeLib.classes.push_back(nativeLib.topClass);
				}
				auto dartFn = nativeLib.topClass->AddFunction(code);
				functions[dartFn->Address()] = dartFn;
			}
		}
		else {
			auto msg = std::format("[!] unknown code at: {:#x}, {}\n", ep_offset, obj.ToCString());
			std::cout << msg;
			std::cout << "  !!! Unhandle case. Please report with your APK\n";
			//throw std::runtime_error(msg);
		}
	}
}

void DartApp::finalizeFunctionsInfo()
{
	auto& parentFn = dart::Function::Handle();
	std::unordered_map<uint64_t, DartFunction*> pending_functions;
	for (auto& [_, dartFn] : functions) {
		// update parent pointer
		if (dartFn->parent) {
			parentFn = dart::FunctionPtr((intptr_t)dartFn->parent);
			const auto ep_addr = parentFn.entry_point() - base();
			if (stubs.contains(ep_addr)) {
				dartFn->parent = nullptr;
			}
			else if (functions.contains(ep_addr)) {
				dartFn->parent = functions[ep_addr];
			}
			else if (pending_functions.contains(ep_addr)) {
				dartFn->parent = pending_functions[ep_addr];
			}
			else {
				auto newDartFn = addFunctionNoCheck(parentFn);
				pending_functions[ep_addr] = newDartFn;
				dartFn->parent = newDartFn;
			}
		}

		// TODO: handle function result type and paramters type
	}

	std::unordered_map<uint64_t, DartFunction*> new_functions;
	while (!pending_functions.empty()) {
		for (auto& [dartFn_ep, dartFn] : pending_functions) {
			if (dartFn->parent) {
				parentFn = dart::FunctionPtr((intptr_t)dartFn->parent);
				const auto ep_addr = parentFn.entry_point() - base();
				if (stubs.contains(ep_addr)) {
					dartFn->parent = nullptr;
				}
				else if (functions.contains(ep_addr)) {
					dartFn->parent = functions[ep_addr];
				}
				else if (pending_functions.contains(ep_addr)) {
					dartFn->parent = pending_functions[ep_addr];
				}
				else if (new_functions.contains(ep_addr)) {
					dartFn->parent = new_functions[ep_addr];
				}
				else {
					auto newDartFn = addFunctionNoCheck(parentFn);
					new_functions[ep_addr] = newDartFn;
					dartFn->parent = newDartFn;
				}
			}
			functions[dartFn_ep] = dartFn;
		}
		pending_functions.clear();
		pending_functions = std::move(new_functions);
		new_functions.clear();
	}

	// null self parent
	for (auto& [_, dartFn] : functions) {
		// update parent pointer
		if (dartFn->parent == dartFn) {
			dartFn->parent = nullptr;
		}
	}

	// extract function parameters
	// Note: Signature is dropped in most function
	auto& func = dart::Function::Handle();
	for (auto& [_, dartFn] : functions) {
		func = dartFn->ptr;
		const auto sigPtr = func.signature();
		if (!sigPtr.IsHeapObject())
			continue;
		const auto& sig = dart::FunctionType::Handle(sigPtr);
		if (!sig.IsNull()) {
			dartFn->Signature().returnType = TypeDb()->FindOrAdd(sig.result_type());

			// function type paramaters
			const auto& type_params = dart::TypeParameters::Handle(sig.type_parameters());
			if (!type_params.IsNull()) {
				// TODO: function type parameters
				//type_params.Print(dart::Thread::Current(), zone, false, 0, dart::Object::kScrubbedName, &buffer);
			}

			const intptr_t num_params = sig.NumParameters();
			const intptr_t num_fixed_params = sig.num_fixed_parameters();
			const intptr_t num_opt_pos_params = sig.NumOptionalPositionalParameters();
			const intptr_t num_opt_named_params = sig.NumOptionalNamedParameters();
			const intptr_t num_opt_params = num_opt_pos_params + num_opt_named_params;

			auto& dname = dart::String::Handle();
			for (intptr_t i = 0; i < num_params; i++) {
				auto dtype = TypeDb()->FindOrAdd(sig.ParameterTypeAt(i));
				auto isRequired = false;
				std::string name;

				if (num_opt_named_params > 0 && i >= num_fixed_params) {
					if (sig.IsRequiredAt(i))
						isRequired = true;
					dname = sig.ParameterNameAt(i);
					name = dname.ToCString();
				}

				dartFn->Signature().params.push_back(FnParam{ dtype, std::move(name), isRequired });
			}
		}
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
		else if (cid == dart::kConstMapCid || cid == dart::kMapCid) {
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
		else if (cid == dart::kConstSetCid || cid == dart::kSetCid) {
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
		else if (obj.IsFunction()) {
			const auto& func = dart::Function::Cast(obj);
			const auto ep_addr = func.entry_point() - base();
			addFunction(ep_addr, func);
		}
		else if (obj.IsClosure()) {
			const auto& closure = dart::Closure::Cast(obj);
			const auto ep_addr = closure.entry_point() - base();
			const auto& func = dart::Function::Handle(closure.function());
			addFunction(ep_addr, func);
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

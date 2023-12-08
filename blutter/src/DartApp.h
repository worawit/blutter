#pragma once
#include "DartLibrary.h"
#include "DartClass.h"
#include "DartFunction.h"
#include "DartStub.h"
#include <unordered_map>

class DartApp
{
public:
	explicit DartApp(const char* path);
	DartApp() = delete;
	~DartApp();

	void EnterScope();
	void ExitScope();

	void LoadInfo();

	intptr_t base() const { return (intptr_t)lib_base; }
	uint32_t offset(intptr_t addr) const { return (uint32_t)(addr - base()); }
	uintptr_t heap_base() const { return heap_base_; }

	DartClass* GetClass(intptr_t cid);
	DartFnBase* GetFunction(uint64_t addr);
	DartField* GetStaticField(intptr_t offset) { return staticFields.at(offset); }

	dart::ObjectPool& GetObjectPool() { return *ppool; }
	DartTypeDb* TypeDb() { return typeDb.get(); }

	intptr_t DartIntCid() const { return dartIntCid; }
	intptr_t DartFutureCid() const { return dartFutureCid; }

private:
	DartLibrary* addLibraryClass(const dart::Library& library, const dart::Class& cls);
	DartLibrary* addLibrary(const dart::Library& library);
	void loadFromClassTable(dart::IsolateGroup* ig);
	void loadStubs(dart::ObjectStore* store);
	DartFunction* addFunctionNoCheck(const dart::Function& func);
	void addFunction(uintptr_t ep_addr, const dart::Function& func);
	void findFunctionInHeap();
	void finalizeFunctionsInfo();
	void loadFromObjectPool();
	void walkObject(dart::Object& obj); // to check field types from existed object

	const void* lib_base;
	const uint8_t* vm_snapshot_data;
	const uint8_t* vm_snapshot_instructions;
	const uint8_t* isolate_snapshot_data;
	const uint8_t* isolate_snapshot_instructions;

	dart::Isolate* isolate;
	uintptr_t heap_base_;
	dart::ObjectPool* ppool;
	bool inScope;

	// nativeLib contains all classes that has no library
	DartLibrary nativeLib;
	std::vector<DartLibrary*> libs;
	// some class might be null
	std::vector<DartClass*> classes;
	std::vector<DartClass*> topClasses;
	std::unordered_map<uint64_t, DartFunction*> functions;
	std::unordered_map<uint64_t, DartStub*> stubs;
	std::unordered_map<uint64_t, DartField*> staticFields;
	std::unique_ptr<DartTypeDb> typeDb;

	// the dart Bulit-in type class id
	intptr_t dartIntCid;
	intptr_t dartDoubleCid;
	intptr_t dartStringCid;
	intptr_t dartBoolCid;
	intptr_t dartRecordCid; // dart verion >= 3.0
	intptr_t dartListCid;
	intptr_t dartSetCid;
	intptr_t dartMapCid;
	intptr_t dartRunesCid;
	intptr_t dartFutureCid;

	intptr_t throwStubAddr;

	friend class CodeAnalyzer;
	friend class DartAnalyzer;
	friend class DartDumper;
	friend class FridaWriter;
};


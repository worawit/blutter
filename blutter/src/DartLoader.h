#pragma once
#include "ElfHelper.h"

class DartLoader final
{
public:
	/*struct LoadInfo {
		void* lib;
		uint8_t* vm_snapshot_data;
		uint8_t* vm_snapshot_instructions;
		uint8_t* isolate_snapshot_data;
		uint8_t* isolate_snapshot_instructions;
		uintptr_t heap_base;

		intptr_t base() { return (intptr_t)lib; }
		uint32_t offset(intptr_t addr) { return (uint32_t)(addr - base()); }
	};*/

	static Dart_Isolate Load(LibAppInfo& libInfo);
	static void Unload();

private:
	DartLoader() = delete;
};


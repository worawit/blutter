#pragma once
#include <stdint.h>

struct LibAppInfo {
	const void* lib;
	const uint8_t* vm_snapshot_data;
	const uint8_t* vm_snapshot_instructions;
	const uint8_t* isolate_snapshot_data;
	const uint8_t* isolate_snapshot_instructions;
	// true for a UNIFIED snapshot (single _kDartSnapshotData/_kDartSnapshotText blob).
	// In that case vm_snapshot_* are null and the isolate_snapshot_* hold the combined blob.
	bool unified = false;
};

class ElfHelper final
{
public:
	static LibAppInfo findSnapshots(const uint8_t* elf);
	static LibAppInfo MapLibAppSo(const char* path);

private:
	ElfHelper() = delete;
};

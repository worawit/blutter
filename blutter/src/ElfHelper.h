#pragma once
#include <stdint.h>

struct LibAppInfo {
	const void* lib;
	const uint8_t* vm_snapshot_data;
	const uint8_t* vm_snapshot_instructions;
	const uint8_t* isolate_snapshot_data;
	const uint8_t* isolate_snapshot_instructions;
};

class ElfHelper final
{
public:
	static LibAppInfo findSnapshots(const uint8_t* elf);
	static LibAppInfo MapLibAppSo(const char* path);

private:
	ElfHelper() = delete;
};

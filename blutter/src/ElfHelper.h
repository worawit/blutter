#pragma once
#include <stdint.h>

struct LibAppInfo {
	const void* lib;
	const uint8_t* vm_snapshot_data;
	const uint8_t* vm_snapshot_instructions;
	const uint8_t* isolate_snapshot_data;
	const uint8_t* isolate_snapshot_instructions;
	size_t file_size;
	// file offsets (= virtual addresses, the image is read at offset 0) of the
	// first and the last byte of executable segments
	size_t text_start;
	size_t text_end;
};

class ElfHelper final
{
public:
	static LibAppInfo findSnapshots(const uint8_t* elf);
	static LibAppInfo MapLibAppSo(const char* path);

private:
	ElfHelper() = delete;
};

#include "pch.h"
#include "MachOHelper.h"
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>

// The Mach-O structures are declared here rather than taken from
// <platform/mach_o.h> because that header only exists in recent Dart SDKs,
// while this file is compiled against every Dart version Blutter supports.
namespace {

constexpr uint32_t MH_MAGIC = 0xfeedface;
constexpr uint32_t MH_CIGAM = 0xcefaedfe;
constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf;
constexpr uint32_t MH_CIGAM_64 = 0xcffaedfe;
// A fat (universal) header is always stored big endian.
constexpr uint32_t FAT_MAGIC = 0xcafebabe;
constexpr uint32_t FAT_CIGAM = 0xbebafeca;
// fat_arch_64 entries are 32 bytes rather than 20, so a 64-bit fat header
// parsed as a 32-bit one yields silently wrong slice offsets.
constexpr uint32_t FAT_MAGIC_64 = 0xcafebabf;
constexpr uint32_t FAT_CIGAM_64 = 0xbfbafeca;

constexpr uint32_t LC_SYMTAB = 0x02;
constexpr uint32_t LC_SEGMENT_64 = 0x19;

// n_type is a symbolic debugging entry when any N_STAB bit is set, otherwise
// the N_TYPE bits say where the symbol lives.
constexpr uint8_t N_STAB = 0xe0;
constexpr uint8_t N_TYPE = 0x0e;
constexpr uint8_t N_SECT = 0x0e;

constexpr int32_t CPU_TYPE_X86_64 = 0x01000007;
constexpr int32_t CPU_TYPE_ARM64 = 0x0100000c;

#pragma pack(push, 1)
struct mach_header_64 {
	uint32_t magic;
	int32_t cputype;
	int32_t cpusubtype;
	uint32_t filetype;
	uint32_t ncmds;
	uint32_t sizeofcmds;
	uint32_t flags;
	uint32_t reserved;
};

struct load_command {
	uint32_t cmd;
	uint32_t cmdsize;
};

struct segment_command_64 {
	uint32_t cmd;
	uint32_t cmdsize;
	char segname[16];
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
	int32_t maxprot;
	int32_t initprot;
	uint32_t nsects;
	uint32_t flags;
};

struct symtab_command {
	uint32_t cmd;
	uint32_t cmdsize;
	uint32_t symoff;
	uint32_t nsyms;
	uint32_t stroff;
	uint32_t strsize;
};

struct nlist_64 {
	uint32_t n_strx;
	uint8_t n_type;
	uint8_t n_sect;
	uint16_t n_desc;
	uint64_t n_value;
};

struct fat_header {
	uint32_t magic;
	uint32_t nfat_arch;
};

struct fat_arch {
	int32_t cputype;
	int32_t cpusubtype;
	uint32_t offset;
	uint32_t size;
	uint32_t align;
};
#pragma pack(pop)

// Segments are page aligned relative to the image base, so keeping the base
// itself page aligned preserves the alignment the Dart VM expects of the
// instructions image.
constexpr size_t kImageAlignment = 0x10000;

uint32_t bswap32(uint32_t value)
{
	return ((value & 0xff000000) >> 24) | ((value & 0x00ff0000) >> 8) |
		((value & 0x0000ff00) << 8) | ((value & 0x000000ff) << 24);
}

std::vector<uint8_t> read_whole_file(const char* path)
{
	FILE* fp = fopen(path, "rb");
	if (fp == nullptr)
		throw std::invalid_argument(std::string("Mach-O: Cannot open ") + path);

	fseek(fp, 0, SEEK_END);
	const auto size = ftell(fp);
	if (size <= 0) {
		fclose(fp);
		throw std::invalid_argument(std::string("Mach-O: Cannot read ") + path);
	}
	fseek(fp, 0, SEEK_SET);

	std::vector<uint8_t> buffer(static_cast<size_t>(size));
	const auto nread = fread(buffer.data(), 1, buffer.size(), fp);
	fclose(fp);
	if (nread != buffer.size())
		throw std::invalid_argument(std::string("Mach-O: Truncated read of ") + path);

	return buffer;
}

// Reads little endian values out of the file with a bounds check, so a
// malformed image cannot walk us off the end of the buffer.
template <typename T>
const T* at(const std::vector<uint8_t>& file, uint64_t offset, uint64_t count = 1)
{
	if (offset > file.size() || count > (file.size() - offset) / sizeof(T))
		throw std::invalid_argument("Mach-O: Structure outside of the file");
	return reinterpret_cast<const T*>(file.data() + offset);
}

// A universal binary holds several images. Pick the one we can analyze,
// preferring arm64 because that is what Flutter ships for devices.
uint64_t find_slice(const std::vector<uint8_t>& file)
{
	const auto magic = *at<uint32_t>(file, 0);
	if (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64)
		throw std::invalid_argument("Mach-O: universal binary with a 64-bit fat header (FAT_MAGIC_64) is not supported");
	if (magic != FAT_MAGIC && magic != FAT_CIGAM)
		return 0;

	const auto* header = at<fat_header>(file, 0);
	const auto count = bswap32(header->nfat_arch);
	const auto* arches = at<fat_arch>(file, sizeof(fat_header), count);

	uint64_t fallback = 0;
	for (uint32_t i = 0; i < count; i++) {
		const auto cputype = static_cast<int32_t>(bswap32(static_cast<uint32_t>(arches[i].cputype)));
		const uint64_t offset = bswap32(arches[i].offset);
		if (cputype == CPU_TYPE_ARM64)
			return offset;
		if (cputype == CPU_TYPE_X86_64 && fallback == 0)
			fallback = offset;
	}

	if (fallback == 0)
		throw std::invalid_argument("Mach-O: No arm64 or x64 image in the universal binary");
	return fallback;
}

struct Segment {
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
};

} // namespace

bool MachOHelper::IsMachO(const char* path)
{
	FILE* fp = fopen(path, "rb");
	if (fp == nullptr)
		return false;

	uint32_t magic = 0;
	const auto nread = fread(&magic, 1, sizeof(magic), fp);
	fclose(fp);
	if (nread != sizeof(magic))
		return false;

	// Report the unsupported flavours too, so MapLibApp can explain why.
	return magic == MH_MAGIC_64 || magic == MH_CIGAM_64 || magic == MH_MAGIC ||
		magic == MH_CIGAM || magic == FAT_MAGIC || magic == FAT_CIGAM ||
		magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
}

LibAppInfo MachOHelper::MapLibApp(const char* path)
{
	const auto file = read_whole_file(path);
	const auto slice = find_slice(file);

	const auto* header = at<mach_header_64>(file, slice);
	switch (header->magic) {
	case MH_MAGIC_64:
		break;
	case MH_CIGAM_64:
		throw std::invalid_argument("Mach-O: Expected a host endian header");
	case MH_MAGIC:
	case MH_CIGAM:
		throw std::invalid_argument("Mach-O: Support only 64 bits");
	default:
		throw std::invalid_argument("Mach-O: Invalid magic header");
	}

	// Collect the segments to lay out, and the symbol table to look up.
	std::vector<Segment> segments;
	const symtab_command* symtab = nullptr;
	uint64_t vm_start = UINT64_MAX;
	uint64_t vm_end = 0;

	uint64_t cmd_offset = slice + sizeof(mach_header_64);
	for (uint32_t i = 0; i < header->ncmds; i++) {
		const auto* cmd = at<load_command>(file, cmd_offset);
		if (cmd->cmdsize < sizeof(load_command))
			throw std::invalid_argument("Mach-O: Invalid load command size");

		if (cmd->cmd == LC_SEGMENT_64) {
			const auto* seg = at<segment_command_64>(file, cmd_offset);
			// __PAGEZERO is an unmapped guard region of several GB.
			if (seg->vmsize != 0 && strncmp(seg->segname, "__PAGEZERO", sizeof(seg->segname)) != 0) {
				// Validate the file range now; the contents are copied below.
				at<uint8_t>(file, slice + seg->fileoff, seg->filesize);
				segments.push_back(Segment{ seg->vmaddr, seg->vmsize, seg->fileoff, seg->filesize });
				vm_start = std::min(vm_start, seg->vmaddr);
				vm_end = std::max(vm_end, seg->vmaddr + seg->vmsize);
			}
		}
		else if (cmd->cmd == LC_SYMTAB) {
			symtab = at<symtab_command>(file, cmd_offset);
		}

		cmd_offset += cmd->cmdsize;
	}

	if (segments.empty())
		throw std::invalid_argument("Mach-O: No loadable segment");
	if (symtab == nullptr)
		throw std::invalid_argument("Mach-O: Cannot find the symbol table");

	// Lay the segments out at their virtual addresses instead of mapping the
	// file as-is. Unlike an ELF libapp.so, an App binary has a zero filled
	// __DATA segment whose addresses would otherwise collide with __LINKEDIT,
	// and the Dart VM writes into that BSS while loading the snapshot.
	const auto image_size = vm_end - vm_start;
	auto* image = static_cast<uint8_t*>(::operator new(image_size, std::align_val_t{ kImageAlignment }));
	memset(image, 0, image_size);
	for (const auto& seg : segments) {
		if (seg.filesize != 0)
			memcpy(image + (seg.vmaddr - vm_start), file.data() + slice + seg.fileoff, seg.filesize);
	}

	// Find the snapshots. Mach-O keeps them in the regular symbol table, and
	// under the same names the ELF loader looks for.
	const uint8_t* vm_snapshot_data = nullptr;
	const uint8_t* vm_snapshot_instructions = nullptr;
	const uint8_t* isolate_snapshot_data = nullptr;
	const uint8_t* isolate_snapshot_instructions = nullptr;

	const auto* symbols = at<nlist_64>(file, slice + symtab->symoff, symtab->nsyms);
	const auto* strtab = at<char>(file, slice + symtab->stroff, symtab->strsize);
	for (uint32_t i = 0; i < symtab->nsyms; i++) {
		const auto& sym = symbols[i];
		if ((sym.n_type & N_STAB) != 0 || (sym.n_type & N_TYPE) != N_SECT)
			continue;
		if (sym.n_strx >= symtab->strsize)
			continue;
		if (sym.n_value < vm_start || sym.n_value >= vm_end)
			continue;

		const char* name = strtab + sym.n_strx;
		const uint8_t* addr = image + (sym.n_value - vm_start);
		if (strcmp(name, kVmSnapshotDataAsmSymbol) == 0) {
			vm_snapshot_data = addr;
		}
		else if (strcmp(name, kVmSnapshotInstructionsAsmSymbol) == 0) {
			vm_snapshot_instructions = addr;
		}
		else if (strcmp(name, kIsolateSnapshotDataAsmSymbol) == 0) {
			isolate_snapshot_data = addr;
		}
		else if (strcmp(name, kIsolateSnapshotInstructionsAsmSymbol) == 0) {
			isolate_snapshot_instructions = addr;
		}
	}

	if (vm_snapshot_data == nullptr)
		throw std::invalid_argument("Mach-O: Cannot find Dart VM Snapshot Data");
	if (vm_snapshot_instructions == nullptr)
		throw std::invalid_argument("Mach-O: Cannot find Dart VM Snapshot Instructions");
	if (isolate_snapshot_data == nullptr)
		throw std::invalid_argument("Mach-O: Cannot find Dart Isolate Snapshot Data");
	if (isolate_snapshot_instructions == nullptr)
		throw std::invalid_argument("Mach-O: Cannot find Dart Isolate Snapshot Instructions");

	// Note: the image is intentionally never freed. It stays alive for the
	// lifetime of the process, exactly like the ELF file mapping does.
	return LibAppInfo{
		.lib = image,
		.vm_snapshot_data = vm_snapshot_data,
		.vm_snapshot_instructions = vm_snapshot_instructions,
		.isolate_snapshot_data = isolate_snapshot_data,
		.isolate_snapshot_instructions = isolate_snapshot_instructions,
	};
}

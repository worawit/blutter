#pragma once
#include <capstone/capstone.h>
#include <utility>
#ifdef TARGET_ARCH_ARM64
#include "Disassembler_arm64.h"
#endif


// master of disassmbled instructions from capstone
// do not allow copy because this class must free the instructions
class AsmInstructions {
	cs_insn* insns;
	size_t count;

	AsmInstructions(cs_insn* insns, size_t count) : insns(insns), count(count) {}
public:
	AsmInstructions() = delete;
	AsmInstructions(const AsmInstructions&) = delete;
	AsmInstructions(AsmInstructions&& rhs) noexcept : insns(std::exchange(rhs.insns, nullptr)), count(std::exchange(rhs.count, 0)) {}
	AsmInstructions& operator=(const AsmInstructions&) = delete;
	~AsmInstructions() { if (insns) cs_free(insns, count); }

	cs_insn* Insns() { return insns; }
	size_t Count() const { return count; }
	AsmInstruction First() { return AsmInstruction(insns); }
	AsmInstruction Last() { return AsmInstruction(&insns[count - 1]); }
	cs_insn* FirstPtr() { return insns; }
	cs_insn* LastPtr() { return &insns[count - 1]; }
	bool IsFirst(AsmInstruction& insn) { return insn.address() == insns->address; }

	AsmInstruction At(size_t i) { return AsmInstruction(insns + i); }
	cs_insn* Ptr(size_t i) { return &insns[i]; }
	size_t AtIndex(uint64_t addr) {
		ASSERT(addr > insns->address);
		// estimate index (normally 4 bytes per instruction for arm64)
		auto idx = (addr - insns->address) / 4;
		ASSERT(idx < count);
		while (idx < count && insns[idx].address < addr)
			++idx;
		return idx;
	}
	AsmInstruction AtAddr(uint64_t addr) {
		ASSERT(addr > insns->address);
		// estimate index (normally 4 bytes per instruction for arm64)
		auto idx = (addr - insns->address) / 4;
		ASSERT(idx < count);
		auto insn = &insns[idx];
		while (insn->address < addr)
			++insn;
		ASSERT(insn->address == addr);
		return AsmInstruction(insn);
	}

	friend class Disassembler;
	friend class Instruction;
};

// partial instructions from Instructions object.
// this class object are safe to copy/move because there is no freeing when destructor is called
class AsmBlock {
	cs_insn* insns;
	cs_insn* last_insn;

public:
	explicit AsmBlock() : insns(nullptr), last_insn(nullptr) {}
	explicit AsmBlock(cs_insn* insns, cs_insn* last_insn) : insns(insns), last_insn(last_insn) {}
	bool isValid() { return insns != nullptr; }
	AsmInstruction first() { return AsmInstruction(insns); }
	AsmInstruction last() { return AsmInstruction(last_insn); }
	cs_insn* first_ptr() { return insns; }
	cs_insn* last_ptr() { return last_insn; }
	AsmInstruction at(size_t i) { return AsmInstruction(insns + i); }
	cs_insn* ptr(size_t i) { return &insns[i]; }
	bool isLast(cs_insn* insn) { return insn == last_insn; }
	bool isAfter(cs_insn* insn) { return insn->address > last_insn->address; }
	uint64_t Address() { return insns->address; }
	uint64_t AddressEnd() { return last_insn->address + last_insn->size; }

	friend class Instruction;
};

class Disassembler
{
public:
	Disassembler(bool hasDetail = true);
	~Disassembler() { cs_close(&cshandle); }
	Disassembler(const Disassembler&) = delete;
	Disassembler(Disassembler&&) = delete;
	Disassembler& operator=(const Disassembler&) = delete;

	AsmInstructions Disasm(const uint8_t* code, size_t code_size, uint64_t address, size_t max_count = 0);
	const char* GetRegName(arm64_reg reg) { return cs_reg_name(cshandle, reg); }

private:
	csh cshandle;
};


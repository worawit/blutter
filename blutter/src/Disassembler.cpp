#include "pch.h"
#include "Disassembler.h"

AsmInstructions Disassembler::Disasm(const uint8_t* code, size_t code_size, uint64_t address, size_t max_count)
{
	cs_insn* insns = NULL;
	size_t insn_cnt = 0;

	insn_cnt = cs_disasm(cshandle, code, code_size, address, max_count, &insns);

	return AsmInstructions(insns, insn_cnt);
}
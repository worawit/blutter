#include "pch.h"
#include "Disassembler.h"

namespace A64 {
const char* Register::RegisterNames[] = {
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9",
	"r10", "r11", "r12", "r13", "r14", "rSP", "r16", "r17", "r18", "r19",
	"r20", "r21", "rNULL", "r23", "r24", "r25", "rTHR", "rPP", "rHEAP", "rFP",
	"r30", "r31",
	"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9",
	"d10", "d11", "d12", "d13", "d14", "d15", "d16", "d17", "d18", "d19",
	"d20", "d21", "d22", "d23", "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31",
	"rCSP", "rZR", "NZCV",
};
}

// singleton of capstone handle. use for global resolve register name  of aarch64
static csh g_cshandle;
const char* GetCsRegisterName(arm64_reg reg)
{
	if (g_cshandle == 0)
		cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &g_cshandle);
	return cs_reg_name(g_cshandle, reg);
}

Disassembler::Disassembler(bool hasDetail)
{
	if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &cshandle) != CS_ERR_OK)
		throw std::runtime_error("Cannot open capstone engine");

	if (hasDetail)
		cs_option(cshandle, CS_OPT_DETAIL, CS_OPT_ON);
}

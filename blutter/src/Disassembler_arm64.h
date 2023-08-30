#pragma once
#include "Disassembler.h"

constexpr arm64_reg ToCapstoneReg(dart::Register r)
{
#define REG_CASE(n) case dart::Register::R##n: \
		return ARM64_REG_X##n

	switch (r) {
		REG_CASE(0); REG_CASE(1); REG_CASE(2); REG_CASE(3); REG_CASE(4);
		REG_CASE(5); REG_CASE(6); REG_CASE(7); REG_CASE(8); REG_CASE(9);
		REG_CASE(10); REG_CASE(11); REG_CASE(12); REG_CASE(13); REG_CASE(14);
		REG_CASE(15); REG_CASE(16); REG_CASE(17); REG_CASE(18); REG_CASE(19);
		REG_CASE(20); REG_CASE(21); REG_CASE(22); REG_CASE(23); REG_CASE(24);
		REG_CASE(25); REG_CASE(26); REG_CASE(27); REG_CASE(28); REG_CASE(29);
		REG_CASE(30);
	default:
		return ARM64_REG_INVALID;
	}
#undef REG_CASE
}

constexpr arm64_reg ToCapstoneReg(arm64_reg r)
{
#define REG_CASE(n) case ARM64_REG_X##n: \
	case ARM64_REG_W##n: \
		return ARM64_REG_X##n

	switch (r) {
		REG_CASE(0); REG_CASE(1); REG_CASE(2); REG_CASE(3); REG_CASE(4);
		REG_CASE(5); REG_CASE(6); REG_CASE(7); REG_CASE(8); REG_CASE(9);
		REG_CASE(10); REG_CASE(11); REG_CASE(12); REG_CASE(13); REG_CASE(14);
		REG_CASE(15); REG_CASE(16); REG_CASE(17); REG_CASE(18); REG_CASE(19);
		REG_CASE(20); REG_CASE(21); REG_CASE(22); REG_CASE(23); REG_CASE(24);
		REG_CASE(25); REG_CASE(26); REG_CASE(27); REG_CASE(28); REG_CASE(29);
		REG_CASE(30);
	case ARM64_REG_XZR:
	case ARM64_REG_WZR:
		return ARM64_REG_XZR;
	case ARM64_REG_SP:
		return ARM64_REG_SP;
	default:
		return ARM64_REG_INVALID;
	}
#undef REG_CASE
}

constexpr dart::Register ToDartReg(arm64_reg r)
{
#define REG_CASE(n) case ARM64_REG_X##n: \
	case ARM64_REG_W##n: \
		return dart::Register::R##n

	switch (r) {
		REG_CASE(0); REG_CASE(1); REG_CASE(2); REG_CASE(3); REG_CASE(4);
		REG_CASE(5); REG_CASE(6); REG_CASE(7); REG_CASE(8); REG_CASE(9);
		REG_CASE(10); REG_CASE(11); REG_CASE(12); REG_CASE(13); REG_CASE(14);
		REG_CASE(15); REG_CASE(16); REG_CASE(17); REG_CASE(18); REG_CASE(19);
		REG_CASE(20); REG_CASE(21); REG_CASE(22); REG_CASE(23); REG_CASE(24);
		REG_CASE(25); REG_CASE(26); REG_CASE(27); REG_CASE(28); REG_CASE(29);
		REG_CASE(30);
	case ARM64_REG_XZR:
	case ARM64_REG_WZR:
		return dart::Register::ZR;
	case ARM64_REG_SP:
		return dart::Register::CSP;
	default:
		return dart::Register::kNoRegister;
	}
#undef REG_CASE
}

constexpr arm64_reg CSREG_ARGS_DESC = ToCapstoneReg(dart::ARGS_DESC_REG);
constexpr arm64_reg CSREG_DART_SP = ToCapstoneReg(dart::SPREG);
constexpr arm64_reg CSREG_DART_FP = ToCapstoneReg(dart::FPREG);
constexpr arm64_reg CSREG_DART_LR = ToCapstoneReg(dart::Register::R30); // LR is now allowed use directly in Dart
constexpr arm64_reg CSREG_DART_DISPATCH_TABLE = ToCapstoneReg(dart::DISPATCH_TABLE_REG);
constexpr arm64_reg CSREG_DART_NULL = ToCapstoneReg(dart::NULL_REG);
constexpr arm64_reg CSREG_DART_WB_OBJECT = ToCapstoneReg(dart::kWriteBarrierObjectReg);
constexpr arm64_reg CSREG_DART_WB_VALUE = ToCapstoneReg(dart::kWriteBarrierValueReg);
constexpr arm64_reg CSREG_DART_WB_SLOT = ToCapstoneReg(dart::kWriteBarrierSlotReg);
constexpr arm64_reg CSREG_DART_THR = ToCapstoneReg(dart::THR);
constexpr arm64_reg CSREG_DART_PP = ToCapstoneReg(dart::PP);
constexpr arm64_reg CSREG_DART_HEAP = ToCapstoneReg(dart::HEAP_BITS);
constexpr arm64_reg CSREG_DART_TMP = ToCapstoneReg(dart::TMP);
constexpr arm64_reg CSREG_DART_TMP2 = ToCapstoneReg(dart::TMP2);
// Note: kTagReg is normally in wrapper function. can ignore it.
constexpr arm64_reg CSREG_ALLOCATE_OBJ_TYPEARGS = ToCapstoneReg(dart::AllocateObjectABI::kTypeArgumentsReg);
constexpr arm64_reg CSREG_ALLOCATE_CLOSURE_FUNCTION = ToCapstoneReg(dart::AllocateClosureABI::kFunctionReg);
constexpr arm64_reg CSREG_ALLOCATE_CLOSURE_CONTEXT = ToCapstoneReg(dart::AllocateClosureABI::kContextReg);
constexpr arm64_reg CSREG_ALLOCATE_CLOSURE_SCRATCH = ToCapstoneReg(dart::AllocateClosureABI::kScratchReg);

const char* GetCsRegisterName(arm64_reg reg);

inline uint32_t GetCsRegSize(arm64_reg reg) {
	if ((reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30) || reg == ARM64_REG_WZR)
		return 4;
	// assume Xnn regsiter
	return 8;
}

namespace A64 {
// copy from dart constants_arm64.h by merging VRegister into Register
enum Register : int32_t {
	R0 = 0,
	R1 = 1,
	R2 = 2,
	R3 = 3,
	R4 = 4,
	R5 = 5,
	R6 = 6,
	R7 = 7,
	R8 = 8,
	R9 = 9,
	R10 = 10,
	R11 = 11,
	R12 = 12,
	R13 = 13,
	R14 = 14,
	R15 = 15,  // SP in Dart code.
	R16 = 16,  // IP0 aka TMP
	R17 = 17,  // IP1 aka TMP2
	R18 = 18,  // reserved on iOS, shadow call stack on Fuchsia, TEB on Windows.
	R19 = 19,
	R20 = 20,
	R21 = 21,  // DISPATCH_TABLE_REG (AOT only)
	R22 = 22,  // NULL_REG
	R23 = 23,
	R24 = 24,  // CODE_REG
	R25 = 25,
	R26 = 26,  // THR
	R27 = 27,  // PP
	R28 = 28,  // HEAP_BITS
	R29 = 29,  // FP
	R30 = 30,  // LR
	R31 = 31,  // ZR, CSP
	// ## floating point (Q, D, S, H, B) and vector registers
	V0 = 32, // v0 Volatile; Parameter/scratch register, result register.
	// v1-v7 Volatile; Parameter/scratch register.
	V1 = 33,
	V2 = 34,
	V3 = 35,
	V4 = 36,
	V5 = 37,
	V6 = 38,
	V7 = 39,
	// v8-v15 Non-volatile; Scratch registers
	// Only the bottom 64 bits are non-volatile! [ARM IHI 0055B, 5.1.2]
	V8 = 40,
	V9 = 41,
	V10 = 42,
	V11 = 43,
	V12 = 44,
	V13 = 45,
	V14 = 46,
	V15 = 47,
	// v16-v31 Volatile; Scratch registers.
	V16 = 48,
	V17 = 49,
	V18 = 50,
	V19 = 51,
	V20 = 52,
	V21 = 53,
	V22 = 54,
	V23 = 55,
	V24 = 56,
	V25 = 57,
	V26 = 58,
	V27 = 59,
	V28 = 60,
	V29 = 61,
	V30 = 62,
	V31 = 63,
	kNumberOfRegisters = 64,
	kNoRegister = -1,

	VTMP = V31,

	// These registers both use the encoding R31, but to avoid mistakes we give
	// them different values, and then translate before encoding.
	CSP = 64,
	ZR = 65,
	NZCV = 66, // condition flags

	// Aliases.
	//SP = R15,
	//TMP = R16,
	//TMP2 = R17,
	//IP0 = R16,
	//IP1 = R17,
	//THR = R26,
	//PP = R27,
	//HEAP_BITS = R28,
	FP = R29,
	LR = R30,  // Note: direct access to this constant is not allowed. See above.

};

inline bool IsDecimalRegister(Register reg) {
	return reg >= V0 && reg <= V31;
}

extern const char* RegisterNames[];

inline const char* GetRegisterName(Register reg) {
	ASSERT(reg != kNoRegister);
	return RegisterNames[reg];
}

constexpr Register FromDartReg(dart::Register r) {
	switch (r) {
#define REG_CASE(n) case dart::Register::R##n: \
		return R##n
		REG_CASE(0); REG_CASE(1); REG_CASE(2); REG_CASE(3); REG_CASE(4);
		REG_CASE(5); REG_CASE(6); REG_CASE(7); REG_CASE(8); REG_CASE(9);
		REG_CASE(10); REG_CASE(11); REG_CASE(12); REG_CASE(13); REG_CASE(14);
		REG_CASE(15); REG_CASE(16); REG_CASE(17); REG_CASE(18); REG_CASE(19);
		REG_CASE(20); REG_CASE(21); REG_CASE(22); REG_CASE(23); REG_CASE(24);
		REG_CASE(25); REG_CASE(26); REG_CASE(27); REG_CASE(28); REG_CASE(29);
		REG_CASE(30);
#undef REG_CASE
	case dart::Register::CSP:
		return CSP;
	case dart::Register::ZR:
		return ZR;
	default:
		return kNoRegister;
	}
}

constexpr auto ARGS_DESC_REG = FromDartReg(dart::ARGS_DESC_REG);
constexpr auto SP_REG = FromDartReg(dart::SPREG);
constexpr auto TMP_REG = FromDartReg(dart::TMP);
constexpr auto TMP2_REG = FromDartReg(dart::TMP2);
constexpr auto NULL_REG = FromDartReg(dart::NULL_REG);

// map from capstone register
constexpr Register FromCsReg(arm64_reg r) {
	switch (r) {
#define REG_CASE(n) case ARM64_REG_X##n: \
	case ARM64_REG_W##n: \
		return R##n
		REG_CASE(0); REG_CASE(1); REG_CASE(2); REG_CASE(3); REG_CASE(4);
		REG_CASE(5); REG_CASE(6); REG_CASE(7); REG_CASE(8); REG_CASE(9);
		REG_CASE(10); REG_CASE(11); REG_CASE(12); REG_CASE(13); REG_CASE(14);
		REG_CASE(15); REG_CASE(16); REG_CASE(17); REG_CASE(18); REG_CASE(19);
		REG_CASE(20); REG_CASE(21); REG_CASE(22); REG_CASE(23); REG_CASE(24);
		REG_CASE(25); REG_CASE(26); REG_CASE(27); REG_CASE(28); REG_CASE(29);
		REG_CASE(30);
#undef REG_CASE
	case ARM64_REG_XZR:
	case ARM64_REG_WZR:
		return ZR;
	case ARM64_REG_SP:
		return SP_REG;
	case ARM64_REG_NZCV: // Condition Flags
		return NZCV;
#define REG_CASE(n) case ARM64_REG_V##n: \
	case ARM64_REG_Q##n: \
	case ARM64_REG_D##n: \
	case ARM64_REG_S##n: \
		return V##n
		REG_CASE(0); REG_CASE(1); REG_CASE(2); REG_CASE(3); REG_CASE(4);
		REG_CASE(5); REG_CASE(6); REG_CASE(7); REG_CASE(8); REG_CASE(9);
		REG_CASE(10); REG_CASE(11); REG_CASE(12); REG_CASE(13); REG_CASE(14);
		REG_CASE(15); REG_CASE(16); REG_CASE(17); REG_CASE(18); REG_CASE(19);
		REG_CASE(20); REG_CASE(21); REG_CASE(22); REG_CASE(23); REG_CASE(24);
		REG_CASE(25); REG_CASE(26); REG_CASE(27); REG_CASE(28); REG_CASE(29);
		REG_CASE(30); REG_CASE(31);
#undef REG_CASE
	default:
		return kNoRegister;
	}
}
}; // namespace ARM64


class AsmInstruction {
private:
	// keep this class object small size for cheap copy
	cs_insn* insn;
public:
	class Operands {
		cs_arm64_op* operands;
		Operands(cs_arm64_op* operands) : operands(operands) {}
	public:
		const cs_arm64_op& operator[](size_t idx) const { return operands[idx]; }
		friend class AsmInstruction;
	} ops;

	AsmInstruction(cs_insn* insn) : insn(insn), ops(insn->detail->arm64.operands) {}
	AsmInstruction& operator=(const AsmInstruction&) = default;
	// prefix increment
	AsmInstruction& operator++() {
		++insn;
		ops = insn->detail->arm64.operands;
		return *this;
	}
	AsmInstruction& operator--() {
		--insn;
		ops = insn->detail->arm64.operands;
		return *this;
	}
	AsmInstruction& operator+=(int cnt) {
		insn += cnt;
		ops = insn->detail->arm64.operands;
		return *this;
	}
	AsmInstruction Next() { return AsmInstruction(insn + 1); }
	AsmInstruction Prev() { return AsmInstruction(insn - 1); }

	friend inline bool operator==(const AsmInstruction& lhs, const AsmInstruction& rhs) {
		return lhs.insn->size == rhs.insn->size && memcmp(lhs.insn->bytes, rhs.insn->bytes, lhs.insn->size) == 0;
	}

	bool IsBranch(arm64_cc cond = ARM64_CC_INVALID) { return insn->id == ARM64_INS_B && cc() == cond; }
	bool IsDartArrayLoad() {
		if (writeback())
			return false;
		switch (insn->id) {
		case ARM64_INS_LDUR:
		case ARM64_INS_LDRB:
			return true;
		}
		return false;
	}
	int GetLoadSize() {
		if (insn->id == ARM64_INS_LDRB || insn->id == ARM64_INS_LDURB)
			return 1;
		return GetCsRegSize(ops[0].reg);
	}
	bool IsDartArrayStore() {
		if (writeback())
			return false;
		switch (insn->id) {
		case ARM64_INS_STUR:
		case ARM64_INS_STRB:
			return true;
		}
		return false;
	}
	int GetStoreSize() {
		if (insn->id == ARM64_INS_STRB || insn->id == ARM64_INS_STURB)
			return 1;
		return GetCsRegSize(ops[0].reg);
	}

	cs_insn* ptr() { return insn; }
	uint64_t address() const { return insn->address; }
	uint16_t size() const { return insn->size; }
	uint64_t NextAddress() const { return insn->address + insn->size; }
	unsigned int id() const { return insn->id; }
	arm64_cc cc() const { return insn->detail->arm64.cc; }
	bool writeback() const { return insn->detail->arm64.writeback; }
	uint8_t op_count() const { return insn->detail->arm64.op_count; }

	const char* mnemonic() const { return insn->mnemonic; }
	const char* op_str() const { return insn->op_str; }
};

#pragma once
#include "Disassembler.h"
#include "VarValue.h"

// forward declaration
struct AsmText;

struct AddrRange {
	uint64_t start;
	uint64_t end;

	AddrRange() : start(0), end(0) {}
	AddrRange(uint64_t start, uint64_t end) : start(start), end(end) {}

	bool Has(uint64_t addr) { return addr >= start && addr < end; }
};

class ILInstr {
public:
	enum ILKind {
		Unknown = 0,
		EnterFrame,
		LeaveFrame,
		AllocateStack,
		CheckStackOverflow,
		LoadObject,
		LoadImm,
		DecompressPointer,
		GdtCall,
		Call,
		Return,
		BranchIfSmi,
		LoadClassId,
		LoadTaggedClassIdMayBeSmi,
		BoxInt64,
		LoadInt32,
	};

	ILInstr(const ILInstr&) = delete;
	ILInstr(ILInstr&&) = delete;
	ILInstr& operator=(const ILInstr&) = delete;
	virtual ~ILInstr() {}

	virtual std::string ToString() = 0;
	ILKind Kind() { return kind; }
	uint64_t Start() { return addrRange.start; }
	uint64_t End() { return addrRange.end; }
	AddrRange Range() { return addrRange; }

protected:
	ILInstr(ILKind kind, AddrRange& addrRange) : addrRange(addrRange), kind(kind) {}
	ILInstr(ILKind kind, cs_insn* insn) : addrRange(insn->address, insn->address + insn->size), kind(kind) {}

	AddrRange addrRange;
	ILKind kind;
};

class UnknownInstr : public ILInstr {
public:
	UnknownInstr(cs_insn* insn, AsmText& asm_text) : ILInstr(Unknown, insn), asm_text(asm_text) {}
	UnknownInstr() = delete;
	UnknownInstr(UnknownInstr&&) = delete;
	UnknownInstr& operator=(const UnknownInstr&) = delete;

	virtual std::string ToString() {
		return "unknown";
	}

protected:
	AsmText& asm_text;
};

class EnterFrameInstr : public ILInstr {
public:
	// 2 assembly instructions (stp lr, fp, [sp, 8]!; mov fp, sp)
	EnterFrameInstr(AddrRange addrRange) : ILInstr(EnterFrame, addrRange) {}
	EnterFrameInstr() = delete;
	EnterFrameInstr(EnterFrameInstr&&) = delete;
	EnterFrameInstr& operator=(const EnterFrameInstr&) = delete;

	virtual std::string ToString() {
		return "EnterFrame";
	}
};

class LeaveFrameInstr : public ILInstr {
public:
	LeaveFrameInstr(AddrRange addrRange) : ILInstr(LeaveFrame, addrRange) {}
	LeaveFrameInstr() = delete;
	LeaveFrameInstr(LeaveFrameInstr&&) = delete;
	LeaveFrameInstr& operator=(const LeaveFrameInstr&) = delete;

	virtual std::string ToString() {
		return "LeaveFrame";
	}
};

class AllocateStackInstr : public ILInstr {
public:
	AllocateStackInstr(cs_insn* insn, uint32_t allocSize) : ILInstr(AllocateStack, insn), allocSize(allocSize) {}
	AllocateStackInstr() = delete;
	AllocateStackInstr(AllocateStackInstr&&) = delete;
	AllocateStackInstr& operator=(const AllocateStackInstr&) = delete;

	virtual std::string ToString() {
		return std::format("AllocStack({:#x})", allocSize);
	}
	uint32_t AllocSize() { return allocSize; }

protected:
	uint32_t allocSize;
};

class CheckStackOverflowInstr : public ILInstr {
public:
	// 3 assembly instructions (ldr tmp, [THR, stack_limit_offset]; cmp sp, tmp; b.ls #overflow_branch)
	CheckStackOverflowInstr(AddrRange addrRange, uint64_t overflowBranch) : ILInstr(CheckStackOverflow, addrRange), overflowBranch(overflowBranch) {}
	CheckStackOverflowInstr() = delete;
	CheckStackOverflowInstr(CheckStackOverflowInstr&&) = delete;
	CheckStackOverflowInstr& operator=(const CheckStackOverflowInstr&) = delete;

	virtual std::string ToString() {
		return "CheckStackOverflow";
	}

protected:
	uint64_t overflowBranch;
};

class LoadObjectInstr : public ILInstr {
public:
	LoadObjectInstr(AddrRange addrRange, VarStorage dst, std::shared_ptr<VarItem> val) : ILInstr(LoadObject, addrRange), dst(dst), val(val) {}
	LoadObjectInstr() = delete;
	LoadObjectInstr(LoadObjectInstr&&) = delete;
	LoadObjectInstr& operator=(const LoadObjectInstr&) = delete;

	virtual std::string ToString() {
		return dst.Name() + " = " + val->Name();
	}

	std::shared_ptr<VarItem> GetValue() {
		return val;
	}

protected:
	VarStorage dst;
	std::shared_ptr<VarItem> val;
};

class LoadImmInstr : public ILInstr {
public:
	LoadImmInstr(AddrRange addrRange, A64::Register dstReg, int64_t val) : ILInstr(LoadImm, addrRange), dstReg(dstReg), val(val) {}
	LoadImmInstr() = delete;
	LoadImmInstr(LoadImmInstr&&) = delete;
	LoadImmInstr& operator=(const LoadImmInstr&) = delete;

	virtual std::string ToString() {
		return std::format("{} = {:#x}", A64::GetRegisterName(dstReg), val);
	}

	A64::Register dstReg;
	int64_t val;
};

class DecompressPointerInstr : public ILInstr {
public:
	DecompressPointerInstr(cs_insn* insn, VarStorage dst) : ILInstr(DecompressPointer, insn), dst(dst) {}
	DecompressPointerInstr() = delete;
	DecompressPointerInstr(DecompressPointerInstr&&) = delete;
	DecompressPointerInstr& operator=(const DecompressPointerInstr&) = delete;

	virtual std::string ToString() {
		return "DecompressPointer " + dst.Name();
	}

protected:
	VarStorage dst;
};

class GdtCallInstr : public ILInstr {
public:
	GdtCallInstr(AddrRange addrRange, int64_t offset) : ILInstr(GdtCall, addrRange), offset(offset) {}
	GdtCallInstr() = delete;
	GdtCallInstr(GdtCallInstr&&) = delete;
	GdtCallInstr& operator=(const GdtCallInstr&) = delete;

	virtual std::string ToString() {
		return std::format("GDT[cid_x0 + {:#x}]()", offset);
	}

protected:
	int64_t offset;
};

class CallInstr : public ILInstr {
public:
	CallInstr(cs_insn* insn, DartFnBase* fnBase, uint64_t addr) : ILInstr(Call, insn), fnBase(fnBase), addr(addr) {}
	CallInstr() = delete;
	CallInstr(CallInstr&&) = delete;
	CallInstr& operator=(const CallInstr&) = delete;

	virtual std::string ToString() {
		if (fnBase != nullptr)
			return std::format("{}()", fnBase->Name());
		return std::format("call {:#x}", addr);
	}

	DartFnBase* GetFunction() {
		return fnBase;
	}

	uint64_t GetCallAddress() {
		return addr;
	}

protected:
	DartFnBase* fnBase;
	uint64_t addr;
};

class ReturnInstr : public ILInstr {
public:
	ReturnInstr(cs_insn* insn) : ILInstr(Return, insn) {}
	ReturnInstr() = delete;
	ReturnInstr(ReturnInstr&&) = delete;
	ReturnInstr& operator=(const ReturnInstr&) = delete;

	virtual std::string ToString() {
		return "ret";
	}
};

class BranchIfSmiInstr : public ILInstr {
public:
	BranchIfSmiInstr(cs_insn* insn, A64::Register objReg, int64_t branchAddr) : ILInstr(BranchIfSmi, insn), objReg(objReg), branchAddr(branchAddr) {}
	BranchIfSmiInstr() = delete;
	BranchIfSmiInstr(BranchIfSmiInstr&&) = delete;
	BranchIfSmiInstr& operator=(const BranchIfSmiInstr&) = delete;

	virtual std::string ToString() {
		return std::format("branchIfSmi({}, {:#x})", A64::GetRegisterName(objReg), branchAddr);
	}

	A64::Register objReg;
	int64_t branchAddr;
};

class LoadClassIdInstr : public ILInstr {
public:
	LoadClassIdInstr(AddrRange addrRange, A64::Register objReg, A64::Register cidReg) : ILInstr(LoadClassId, addrRange), objReg(objReg), cidReg(cidReg) {}
	LoadClassIdInstr() = delete;
	LoadClassIdInstr(LoadClassIdInstr&&) = delete;
	LoadClassIdInstr& operator=(const LoadClassIdInstr&) = delete;

	virtual std::string ToString() {
		return std::format("{} = LoadClassIdInstr({})", A64::GetRegisterName(cidReg), A64::GetRegisterName(objReg));
	}

	A64::Register objReg;
	A64::Register cidReg;
};

class LoadTaggedClassIdMayBeSmiInstr : public ILInstr {
public:
	LoadTaggedClassIdMayBeSmiInstr(AddrRange addrRange, std::unique_ptr<LoadImmInstr> il_loadImm, 
		std::unique_ptr<BranchIfSmiInstr> il_branchIfSmi, std::unique_ptr<LoadClassIdInstr> il_loadClassId) 
		: ILInstr(LoadTaggedClassIdMayBeSmi, addrRange), taggedCidReg(il_loadClassId->cidReg), objReg(il_loadClassId->objReg),
		il_loadImm(std::move(il_loadImm)), il_branchIfSmi(std::move(il_branchIfSmi)), il_loadClassId(std::move(il_loadClassId)) {}
	LoadTaggedClassIdMayBeSmiInstr() = delete;
	LoadTaggedClassIdMayBeSmiInstr(LoadTaggedClassIdMayBeSmiInstr&&) = delete;
	LoadTaggedClassIdMayBeSmiInstr& operator=(const LoadTaggedClassIdMayBeSmiInstr&) = delete;

	virtual std::string ToString() {
		return std::format("{} = LoadTaggedClassIdMayBeSmiInstr({})", A64::GetRegisterName(taggedCidReg), A64::GetRegisterName(objReg));
	}

	A64::Register taggedCidReg;
	A64::Register objReg;
	std::unique_ptr<LoadImmInstr> il_loadImm;
	std::unique_ptr<BranchIfSmiInstr> il_branchIfSmi;
	std::unique_ptr<LoadClassIdInstr> il_loadClassId;
};

class BoxInt64Instr : public ILInstr {
public:
	BoxInt64Instr(AddrRange addrRange, A64::Register objReg, A64::Register srcReg) : ILInstr(BoxInt64, addrRange), objReg(objReg), srcReg(srcReg) {}
	BoxInt64Instr() = delete;
	BoxInt64Instr(BoxInt64Instr&&) = delete;
	BoxInt64Instr& operator=(const BoxInt64Instr&) = delete;

	virtual std::string ToString() {
		return std::format("{} = BoxInt64Instr({})", A64::GetRegisterName(objReg), A64::GetRegisterName(srcReg));
	}

	A64::Register objReg;
	A64::Register srcReg;
};

class LoadInt32Instr : public ILInstr {
public:
	LoadInt32Instr(AddrRange addrRange, A64::Register dstReg, A64::Register srcObjReg) : ILInstr(LoadInt32, addrRange), dstReg(dstReg), srcObjReg(srcObjReg) {}
	LoadInt32Instr() = delete;
	LoadInt32Instr(LoadInt32Instr&&) = delete;
	LoadInt32Instr& operator=(const LoadInt32Instr&) = delete;

	virtual std::string ToString() {
		return std::format("{} = LoadInt32Instr({})", A64::GetRegisterName(dstReg), A64::GetRegisterName(srcObjReg));
	}

	A64::Register dstReg;
	A64::Register srcObjReg;
};

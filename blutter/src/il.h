#pragma once
#include "Disassembler.h"
#include "VarValue.h"

// forward declaration
struct AsmText;

struct AddrRange {
	uint64_t start;
	uint64_t end;

	AddrRange(uint64_t start, uint64_t end) : start(start), end(end) {}
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
		DecompressPointer,
		GdtCall,
		Call,
		Return,
		CheckIfSmi,
	};

	ILInstr(const ILInstr&) = delete;
	ILInstr(ILInstr&&) = delete;
	ILInstr& operator=(const ILInstr&) = delete;
	virtual ~ILInstr() {}

	virtual std::string ToString() = 0;
	ILKind Kind() { return kind; }

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
		return std::format("GDT[cid + {:#x}]()", offset);
	}

protected:
	int64_t offset;
};

class CallInstr : public ILInstr {
public:
	CallInstr(cs_insn* insn, DartFnBase& fnBase) : ILInstr(Call, insn), fnBase(fnBase) {}
	CallInstr() = delete;
	CallInstr(CallInstr&&) = delete;
	CallInstr& operator=(const CallInstr&) = delete;

	virtual std::string ToString() {
		return std::format("{}()", fnBase.Name());
	}

	DartFnBase& GetFunction() {
		return fnBase;
	}

protected:
	DartFnBase& fnBase;
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
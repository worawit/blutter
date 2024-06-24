#pragma once
#include "Disassembler.h"
#include "il.h"
#include <array>

// forward declaration
class DartApp;
class DartFunction;

struct AsmText {
	enum DataType : uint8_t {
		None,
		ThreadOffset,
		PoolOffset,
		Boolean,
		Call,
	};

	uint64_t addr;
	char text[71]; // first 16 bytes are for mnemonic and spaces, after that is operands
	uint8_t dataType;
	union {
		uint64_t threadOffset;
		uint64_t poolOffset;
		bool boolVal;
		uint64_t callAddress;
	};
};

class AsmTexts {
public:
	AsmTexts(std::vector<AsmText> asm_texts, uint64_t first_stack_limit_addr, int max_param_stack_offset)
		: first_addr{ asm_texts.front().addr }, last_addr{ asm_texts.back().addr }, first_stack_limit_addr{ first_stack_limit_addr }, 
		  max_param_stack_offset{ max_param_stack_offset }, asm_texts{ std::move(asm_texts) } {}

	std::vector<AsmText>& Data() { return asm_texts; }

	size_t AtIndex(uint64_t addr) {
		ASSERT(addr >= first_addr && addr <= last_addr);
		// TODO: below is specific to arm64
		// estimate index (normally 4 bytes per instruction for arm64)
		auto idx = (addr - first_addr) / 4;
		while (asm_texts[idx].addr < addr)
			++idx;
		return idx;
	}

	AsmText& AtAddr(uint64_t addr) { return asm_texts[AtIndex(addr)]; }

	uint64_t FirstStackLimitAddress() const { return first_stack_limit_addr; }

	int MaxParamStackOffset() const { return max_param_stack_offset; }

private:
	uint64_t first_addr;
	uint64_t last_addr;
	uint64_t first_stack_limit_addr;
	int max_param_stack_offset;
	std::vector<AsmText> asm_texts;
};

struct FnParamInfo {
	A64::Register paramReg; // when parameter is passed with register
	int32_t paramOffset{ 0 }; // offset from FP (first param offset is 0x10. if it is optional param, value is 0)
	A64::Register valReg;
	int32_t localOffset{ 0 }; // offset from FP (local variable)
	DartType* type{ nullptr };
	std::string name;
	std::unique_ptr<VarValue> val;

	explicit FnParamInfo() {}
	explicit FnParamInfo(A64::Register paramReg, A64::Register valReg, int32_t localOffset) : paramReg(paramReg), valReg(valReg), localOffset(localOffset) {}
	explicit FnParamInfo(A64::Register valReg) : valReg(valReg) {}
	explicit FnParamInfo(A64::Register valReg, int32_t localOffset) : valReg(valReg), localOffset(localOffset) {}
	explicit FnParamInfo(A64::Register valReg, std::unique_ptr<VarValue> val) : valReg(valReg), val(std::move(val)) {}
	explicit FnParamInfo(A64::Register valReg, int32_t localOffset, DartType* type, std::string name, std::unique_ptr<VarValue> val)
		: valReg(valReg), localOffset(localOffset), type(type), name(std::move(name)), val(std::move(val)) {}
	explicit FnParamInfo(A64::Register valReg, std::string name) : valReg(valReg), name(std::move(name)) {}
	explicit FnParamInfo(std::string name) : name(std::move(name)) {}

	std::string ToString() const;
};

struct FnParams {
	int NumParam() const { return params.size(); }
	int NumOptionalParam() const { return params.size() - numFixedParam; }
	bool empty() const { return params.empty(); }
	void add(FnParamInfo&& param) { params.push_back(std::move(param)); }
	void addFixedParam(FnParamInfo&& param) { params.push_back(std::move(param)); numFixedParam++; }
	FnParamInfo& back() { return params.back(); }
	FnParamInfo& operator[](int i) { return params[i]; }

	FnParamInfo* findValReg(A64::Register reg);
	bool movValReg(A64::Register dstReg, A64::Register srcReg);
	std::string ToString() const;

	uint8_t numFixedParam{ 0 };
	bool isNamedParam{ false };
	std::vector<FnParamInfo> params;
};

class AnalyzingState {
public:
	AnalyzingState(uint32_t stackSize) : local_vars{ stackSize / sizeof(void*), nullptr } { regs.fill(nullptr); }

	void SetRegister(A64::Register reg, VarValue* val) { regs[reg] = val; }
	void ClearRegister(A64::Register reg) { regs[reg] = nullptr; }
	VarValue* MoveRegister(A64::Register dstReg, A64::Register srcReg) {
		auto val = regs[srcReg];
		if (dstReg != srcReg) {
			regs[dstReg] = val;
			regs[srcReg] = nullptr; // normally, Dart moves register for freeing the src register
		}
		return val;
	}
	VarValue* GetValue(A64::Register reg) { return regs[reg]; }

	static int localOffsetToIndex(int offset) { ASSERT(offset < 0);  return (-offset - sizeof(void*)) / sizeof(void*); }
	static int indexToLocalOffset(int idx) { return -(idx + 1) * sizeof(void*); }
	void SetLocal(int offset, VarValue* val) { local_vars[localOffsetToIndex(offset)] = val; }
	VarValue* GetLocal(int offset) { return local_vars[localOffsetToIndex(offset)]; }

//private:
	// variable storage. only reference here.
	std::array<VarValue*, A64::Register::kNumberOfRegisters> regs;
	std::vector<VarValue*> local_vars;
	std::vector<VarValue*> callee_args;
};

class AnalyzingVars {
public:
	AnalyzingVars() : valArgsDesc(std::make_unique<VarValue>(VarValue::ArgsDesc)), valCurrNumNameParam(std::make_unique<VarValue>(VarValue::CurrNumNameParam)) {}

	VarValue* ValParam(int idx) {
		if (idx >= valParams.size()) {
			int i = (int)valParams.size();
			valParams.resize(idx + 1);
			for (; i < idx + 1; i++)
				valParams[i] = std::make_unique<VarParam>(i);
		}
		return valParams[idx].get();
	}
	VarValue* ValArgsDesc() const { return valArgsDesc.get(); }

	// we need it to suppress an error about use without define.
	VarValue* ValCurrNumNameParam() const { return valCurrNumNameParam.get(); }

	// pending load value ILs for variables initialization in prologue
	std::vector<std::unique_ptr<ILInstr>> pending_ils;
private:
	// VarValue of function parameter owner
	std::vector<std::unique_ptr<VarValue>> valParams;
	std::unique_ptr<VarValue> valArgsDesc;
	std::unique_ptr<VarValue> valCurrNumNameParam;
};

class AnalyzedFnData {
public:
	AnalyzedFnData(DartApp& app, DartFunction& dartFn, AsmTexts asmTexts);
	void AddIL(std::unique_ptr<ILInstr> insn) {
		il_insns.push_back(std::move(insn));
	}

	ILInstr* LastIL() {
		return il_insns.back().get();
	}

	void RemoveLastIL() {
		il_insns.pop_back();
	}

	DartApp& app;
	DartFunction& dartFn;
	AsmTexts asmTexts;
	cs_insn* last_ret{ nullptr };
	uint32_t stackSize{ 0 }; // for local variables (this includes space for call arguments)
	bool useFramePointer{ false };
	uint64_t firstCheckStackOverflowAddr{ 0 };
	FnParams params;
	std::vector<std::unique_ptr<ILInstr>> il_insns;
	DartType* returnType{ nullptr };

	//int firstParamOffset{ 0 };
	// TODO: initialization list in prologue, type argument (from ArgumentsDescriptor or Closure)
	A64::Register closureContextReg;
	int32_t closureContextLocalOffset{ 0 }; // offset from FP (local variable)
	// type argument from ArgumentsDescriptor
	A64::Register typeArgumentReg;
	int32_t typeArgumentLocalOffset{ 0 };

	void InitState() { state = std::make_unique<AnalyzingState>(stackSize); }
	void DestroyState() { state.release(); }
	AnalyzingState* State() const { return state.get(); }
	void InitVars() { vars = std::make_unique<AnalyzingVars>(); }
	void DestroyVars() { vars.release(); }
	AnalyzingVars* Vars() const { return vars.get(); }

private:
	std::unique_ptr<AnalyzingVars> vars;
	std::unique_ptr<AnalyzingState> state;

	friend class CodeAnalyzer;
};

class CodeAnalyzer
{
public:
	CodeAnalyzer(DartApp& app) : app(app) {};

	void AnalyzeAll();

private:
	static AsmTexts convertAsm(AsmInstructions& asm_insns);
	
	// implementation is specific to architecture
	void asm2il(DartFunction* dartFn, AsmInstructions& asm_insns);

	DartApp& app;
};

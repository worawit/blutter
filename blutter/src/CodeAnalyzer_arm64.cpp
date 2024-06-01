#include "pch.h"
#include "CodeAnalyzer.h"
#include "DartApp.h"
#include "VarValue.h"
#include "DartThreadInfo.h"
#include <source_location>

#ifndef NO_CODE_ANALYSIS

// auto revert ASM iterator to the current when pattern does not match or an exception occurs
class InsnMarker
{
public:
	explicit InsnMarker(AsmIterator& insn) : insn(insn), mark(insn.Current()) {}
	~InsnMarker() {
		if (mark)
			insn.SetCurrent(mark);
	}

	int64_t Take() {
		auto res = mark->address;
		mark = nullptr;
		return res;
	}

	cs_insn* Insn() {
		return mark;
	}

private:
	AsmIterator& insn;
	cs_insn* mark;
};

class InsnException
{
public:
	explicit InsnException(const char* cond, AsmIterator& insn, const std::source_location& location = std::source_location::current())
		: cond{ cond }, insn{ insn.Current() }, location{ location } {}

	std::string cond;
	cs_insn* insn;
	std::source_location location;
};

#define INSN_ASSERT(cond) \
  do {                    \
	if (!(cond)) throw InsnException(#cond, insn); \
  } while (false)

static VarValue* getPoolObject(DartApp& app, intptr_t offset, A64::Register dstReg)
{
	intptr_t idx = dart::ObjectPool::IndexFromOffset(offset);
	auto& pool = app.GetObjectPool();
	auto objType = pool.TypeAt(idx);
	// see how the EntryType is handled from vm/object_service.cc - ObjectPool::PrintJSONImpl()
	if (objType == dart::ObjectPool::EntryType::kTaggedObject) {
		//val = (uintptr_t)pool.ObjectAt(idx);
		auto ptr = pool.ObjectAt(idx);
		// Smi is special case. Have to handle first
		if (!ptr.IsHeapObject()) {
			return new VarInteger(dart::RawSmiValue(dart::Smi::RawCast(ptr)), dart::kSmiCid);
		}

		if (ptr.IsRawNull())
			return new VarNull();

		auto& obj = dart::Object::Handle(ptr);
		if (obj.IsString())
			return new VarString(dart::String::Cast(obj).ToCString());

		// use TypedData or TypedDataBase ?
		if (obj.IsTypedData()) {
			//dart::kTypedDataInt32ArrayCid;
			//auto& data = dart::TypedData::Cast(obj);
			return new VarExpression(std::format("{}", obj.ToCString()), (int32_t)obj.GetClassId());
		}

		switch (obj.GetClassId()) {
		case dart::kSmiCid:
			return new VarInteger(dart::Smi::Cast(obj).Value(), dart::kSmiCid);
		case dart::kMintCid:
			return new VarInteger(dart::Mint::Cast(obj).AsInt64Value(), dart::kMintCid);
		case dart::kDoubleCid:
			return new VarDouble(dart::Double::Cast(obj).value());
		case dart::kBoolCid:
			return new VarBoolean(dart::Bool::Cast(obj).value());
		case dart::kNullCid:
			return new VarNull();
		case dart::kCodeCid: {
			const auto& code = dart::Code::Cast(obj);
			auto stub = app.GetFunction(code.EntryPoint() - app.base());
			ASSERT(stub);
			return new VarFunctionCode(*stub);
		}
		case dart::kFieldCid: {
			const auto& field = dart::Field::Cast(obj);
			auto dartCls = app.GetClass(field.Owner().untag()->id());
			auto dartField = dartCls->FindField(field.TargetOffset());
			//auto dartField = app.GetStaticField(field.TargetOffset());
			ASSERT(dartField);
			return new VarField(*dartField);
		}
		case dart::kImmutableArrayCid:
			return new VarArray(dart::Array::Cast(obj).ptr());
		// should function and closure be their var types?
		case dart::kFunctionCid:
		case dart::kClosureCid:
		case dart::kConstMapCid:
			// TODO: map
		case dart::kConstSetCid:
			// TODO: set
			return new VarExpression(std::format("{}", obj.ToCString()), (int32_t)obj.GetClassId());
#ifdef HAS_RECORD_TYPE
		case dart::kRecordCid: {
			// temporary expression for Record object (need full object for analysis)
			//const auto& rec = dart::Record::Cast(obj);
			return new VarExpression(std::format("{}", obj.ToCString()), (int32_t)obj.GetClassId());
		}
#endif
		case dart::kTypeParametersCid:
			throw std::runtime_error("Type parameter in Object Pool");
		case dart::kTypeCid:
			return new VarType(*app.TypeDb()->FindOrAdd(dart::Type::Cast(obj).ptr()));
#ifdef HAS_RECORD_TYPE
		case dart::kRecordTypeCid:
			return new VarRecordType(*app.TypeDb()->FindOrAdd(dart::RecordType::Cast(obj).ptr()));
#endif
		case dart::kTypeParameterCid:
			return new VarTypeParameter(*app.TypeDb()->FindOrAdd(dart::TypeParameter::Cast(obj).ptr()));
		case dart::kFunctionTypeCid:
			return new VarFunctionType(*app.TypeDb()->FindOrAdd(dart::FunctionType::Cast(obj).ptr()));
		case dart::kTypeArgumentsCid: {
			return new VarTypeArgument(*app.TypeDb()->FindOrAdd(dart::TypeArguments::Cast(obj).ptr()));
		}
		case dart::kSentinelCid:
			return new VarSentinel();
		case dart::kUnlinkedCallCid: {
			intptr_t idx = dart::ObjectPool::IndexFromOffset(offset + 8);
			ASSERT(pool.TypeAt(idx) == dart::ObjectPool::EntryType::kImmediate);
			auto imm = pool.RawValueAt(idx);
			auto dartFn = app.GetFunction(imm - app.base());
			return new VarUnlinkedCall(*dartFn->AsStub());
		}
		case dart::kSubtypeTestCacheCid:
			return new VarSubtypeTestCache();
		case dart::kLibraryPrefixCid:
			// TODO: handle LibraryPrefix object
		case dart::kInstanceCid:
			return new VarInstance(app.GetClass(dart::kInstanceCid));
		}

		if (obj.IsInstance()) {
			auto dartCls = app.GetClass(obj.GetClassId());
			if (dartCls->Id() < dart::kNumPredefinedCids) {
				std::cerr << std::format("Unhandle predefined class {} ({})\n", dartCls->Name(), dartCls->Id());
			}
			return new VarInstance(dartCls);
		}

		throw std::runtime_error("unhandle object class in getPoolObject");
		//auto txt = std::format("XXX: {}", obj.ToCString());
		//return new VarExpression(txt);
	}
	else if (objType == dart::ObjectPool::EntryType::kImmediate) {
		auto imm = pool.RawValueAt(idx);
		if (dstReg.IsDecimal())
			return new VarDouble(*((double*)&imm), VarType::NativeDouble);
		return new VarInteger(imm, VarValue::NativeInt);
	}
	else if (objType == dart::ObjectPool::EntryType::kNativeFunction) {
		//val = pool.RawValueAt(idx);
		throw std::runtime_error("getting native function pool object from Dart code");
	}
	else {
		throw std::runtime_error(std::format("unknown pool object type: {}", (int)objType).c_str());
	}
}

static inline void handleDecompressPointer(AsmIterator& insn, arm64_reg reg) {
	INSN_ASSERT(insn.id() == ARM64_INS_ADD);
	INSN_ASSERT(insn.ops(0).reg == insn.ops(1).reg && insn.ops(0).reg == reg);
	INSN_ASSERT(insn.ops(2).reg == CSREG_DART_HEAP && insn.ops(2).shift.value == 32);
	++insn;
}

struct ILResult {
	cs_insn* lastIns{ nullptr };
	std::unique_ptr<ILInstr> il;
	uint64_t NextAddress() const { return lastIns->address + lastIns->size; }
	// TODO: ASSERT(il->Kind())
	template <typename T, typename = std::enable_if<std::is_base_of<ILInstr, T>::value>>
	T* get() { return reinterpret_cast<T*>(il.get()); }
};
struct StoreLocalResult {
	arm64_reg srcReg{ ARM64_REG_INVALID };
	int fpOffset{ 0 };
};

class FunctionAnalyzer
{
public:
	FunctionAnalyzer(AnalyzedFnData* fnInfo, DartFunction* dartFn, AsmInstructions& asm_insns, DartApp& app)
		: fnInfo{ fnInfo }, dartFn{ dartFn }, asm_insns{ asm_insns }, app{ app } {}

	void asm2il();

	// returns an instruction after the prologue
	void handlePrologue(AsmIterator& insItr, uint64_t endPrologueAddr);
	std::tuple<A64::Register, A64::Register> unboxParam(AsmIterator& insn, A64::Register expectedSrcReg = A64::Register{});
	void handleFixedParameters(AsmIterator& insn, arm64_reg paramCntReg, int paramCnt = 0);
	void handleOptionalPositionalParameters(AsmIterator& insn, arm64_reg optionalParamCntReg);
	void handleOptionalNamedParameters(AsmIterator& insn, arm64_reg paramCntReg);
	void handleArgumentsDescriptorTypeArguments(AsmIterator& insn);

	StoreLocalResult handleStoreLocal(AsmIterator& insn, arm64_reg expected_src_reg = ARM64_REG_INVALID);

	std::unique_ptr<SetupParametersInstr> processPrologueParametersInstr(AsmIterator& insn, uint64_t endPrologueAddr);

	std::unique_ptr<EnterFrameInstr> processEnterFrameInstr(AsmIterator& insn);
	std::unique_ptr<LeaveFrameInstr> processLeaveFrameInstr(AsmIterator& insn);
	std::unique_ptr<AllocateStackInstr> processAllocateStackInstr(AsmIterator& insn);
	std::unique_ptr<CheckStackOverflowInstr> processCheckStackOverflowInstr(AsmIterator& insn);
	std::unique_ptr<CallLeafRuntimeInstr> processCallLeafRuntime(AsmIterator& insn);
	std::unique_ptr<LoadValueInstr> processLoadValueInstr(AsmIterator& insn);
	std::unique_ptr<ClosureCallInstr> processClosureCallInstr(AsmIterator& insn);
	std::unique_ptr<MoveRegInstr> processMoveRegInstr(AsmIterator& insn);
	std::unique_ptr<DecompressPointerInstr> processDecompressPointerInstr(AsmIterator& insn);
	std::unique_ptr<SaveRegisterInstr> processSaveRegisterInstr(AsmIterator& insn);
	std::unique_ptr<RestoreRegisterInstr> processLoadSavedRegisterInstr(AsmIterator& insn);
	std::unique_ptr<InitAsyncInstr> processInitAsyncInstr(AsmIterator& insn);
	std::unique_ptr<CallInstr> processCallInstr(AsmIterator& insn);
	std::unique_ptr<GdtCallInstr> processGdtCallInstr(AsmIterator& insn);
	std::unique_ptr<ReturnInstr> processReturnInstr(AsmIterator& insn);
	std::unique_ptr<TestTypeInstr> processInstanceofNoTypeArgumentInstr(AsmIterator& insn);
	std::unique_ptr<BranchIfSmiInstr> processBranchIfSmiInstr(AsmIterator& insn);
	std::unique_ptr<LoadClassIdInstr> processLoadClassIdInstr(AsmIterator& insn);
	std::unique_ptr<BoxInt64Instr> processBoxInt64Instr(AsmIterator& insn);
	std::unique_ptr<LoadInt32Instr> processLoadInt32FromBoxOrSmiInstrFromSrcReg(AsmIterator& insn, A64::Register expectedSrcReg = A64::Register{});
	std::unique_ptr<LoadInt32Instr> processLoadInt32FromBoxOrSmiInstr(AsmIterator& insn);
	std::unique_ptr<LoadTaggedClassIdMayBeSmiInstr> processLoadTaggedClassIdMayBeSmiInstr(AsmIterator& insn);
	std::unique_ptr<ILInstr> processLoadFieldTableInstr(AsmIterator& insn);
	std::unique_ptr<AllocateObjectInstr> processTryAllocateObject(AsmIterator& insn);
	std::unique_ptr<WriteBarrierInstr> processWriteBarrierInstr(AsmIterator& insn);
	std::unique_ptr<ILInstr> processLoadStore(AsmIterator& insn);

	struct ObjectPoolInstr {
		A64::Register dstReg;
		VarItem item{};
		bool IsSet() const { return dstReg.IsSet(); }
	};

private:
	void setAsmTextDataPool(uint64_t addr, uint64_t offset) {
		auto& asm_text = fnInfo->asmTexts.AtAddr(addr);
		// TODO: NativeDouble and NativeInt
		asm_text.dataType = AsmText::PoolOffset;
		asm_text.poolOffset = offset;
	}
	void setAsmTextDataBoolean(uint64_t addr, bool b) {
		auto& asm_text = fnInfo->asmTexts.AtAddr(addr);
		asm_text.dataType = AsmText::Boolean;
		asm_text.boolVal = b;
	}
	void setAsmTextDataCall(uint64_t addr, uint64_t callAddress) {
		auto& asm_text = fnInfo->asmTexts.AtAddr(addr);
		asm_text.dataType = AsmText::Call;
		asm_text.callAddress = callAddress;
	}

	ObjectPoolInstr getObjectPoolInstruction(AsmIterator& insn);
	void printInsnException(InsnException& e);

	AnalyzedFnData* fnInfo;
	DartFunction* dartFn;
	AsmInstructions& asm_insns;
	DartApp& app;
};

typedef std::unique_ptr<ILInstr>(FunctionAnalyzer::* AsmMatcherFn)(AsmIterator& insn);
static const AsmMatcherFn matcherFns[] = {
	(AsmMatcherFn) &FunctionAnalyzer::processEnterFrameInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLeaveFrameInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processAllocateStackInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processCheckStackOverflowInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processCallLeafRuntime,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadValueInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processDecompressPointerInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processClosureCallInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processSaveRegisterInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadSavedRegisterInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processInitAsyncInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processCallInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processGdtCallInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processReturnInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processInstanceofNoTypeArgumentInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processBranchIfSmiInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadClassIdInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processBoxInt64Instr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadInt32FromBoxOrSmiInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadTaggedClassIdMayBeSmiInstr,
	&FunctionAnalyzer::processLoadFieldTableInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processTryAllocateObject,
	(AsmMatcherFn) &FunctionAnalyzer::processWriteBarrierInstr,
	&FunctionAnalyzer::processLoadStore,
};

FunctionAnalyzer::ObjectPoolInstr FunctionAnalyzer::getObjectPoolInstruction(AsmIterator& insn)
{
	int64_t offset = 0;
	A64::Register dstReg;
	InsnMarker marker(insn);
	if (insn.id() == ARM64_INS_LDR) {
		// PP offset less than 12 bits
		if (insn.ops(1).mem.base == CSREG_DART_PP && insn.ops(1).mem.index == ARM64_REG_INVALID) {
			offset = insn.ops(1).mem.disp;
			dstReg = A64::Register{ insn.ops(0).reg };
			++insn;
		}
	}
	else if (insn.id() == ARM64_INS_ADD) {
		// special case from ARM64 because true and false are loaded from NULL instead of Object Pool
		if (insn.ops(1).reg == CSREG_DART_NULL) {
			// In Assembler::LoadObjectHelper() of arm64 only. load true and false from null
			ASSERT(insn.ops(2).type == ARM64_OP_IMM);
			const auto offset = insn.ops(2).imm;
			bool val;
			if (offset == dart::kTrueOffsetFromNull) {
				val = true;
			}
			else if (offset == dart::kFalseOffsetFromNull) {
				val = false;
			}
			else {
				FATAL("add from NULL_REG");
			}
			auto b = new VarBoolean(val);
			setAsmTextDataBoolean(marker.Take(), val);
			dstReg = A64::Register{ insn.ops(0).reg };
			++insn;
			return ObjectPoolInstr{ dstReg, VarItem{ VarStorage::NewImmediate(), b } };
		}

		if (insn.ops(1).reg == CSREG_DART_PP && insn.ops(2).type == ARM64_OP_IMM && insn.ops(2).shift.type == ARM64_SFT_LSL && insn.ops(2).shift.value == 12) {
			auto base = insn.ops(2).imm << 12;
			auto offset_reg = insn.ops(0).reg;
			++insn;

			if (insn.id() == ARM64_INS_LDR) {
				INSN_ASSERT(insn.ops(1).mem.base == offset_reg);
				offset = base + insn.ops(1).mem.disp;
			}
			else if (insn.id() == ARM64_INS_LDP) {
				INSN_ASSERT(insn.ops(2).mem.base == offset_reg);
				offset = base + insn.ops(2).mem.disp;
			}
			else if (insn.id() == ARM64_INS_ADD) {
				// use when loading pair from object pool (local 2 entries)
				// see it for UnlinkedCall by the next entry is the jump address
				INSN_ASSERT(insn.ops(2).type == ARM64_OP_IMM);
				offset = base + insn.ops(2).imm;
			}
			else {
				INSN_ASSERT(false);
			}
			dstReg = A64::Register{ insn.ops(0).reg };
			++insn;
		}
	}
	else if (insn.id() == ARM64_INS_MOV) {
		// more than 20 bits (never seen it)
		// MOV X5, #offset_low
		// MOVK X5, #offset_high LSL#16
		// LDR X6, [X27,X5]
		const auto offset_reg = insn.ops(0).reg;
		offset = insn.ops(1).imm;
		++insn;

		if (insn.id() == ARM64_INS_MOVK && insn.ops(0).reg == offset_reg &&
			insn.ops(1).type == ARM64_OP_IMM && insn.ops(1).shift.type == ARM64_SFT_LSL && insn.ops(1).shift.value == 16)
		{
			offset |= insn.ops(1).imm << 16;
			++insn;

			if (insn.id() == ARM64_INS_LDR && insn.ops(1).mem.base == CSREG_DART_PP && insn.ops(1).mem.index == offset_reg) {
				dstReg = A64::Register{ insn.ops(0).reg };
				++insn;
			}
		}
	}

	if (!dstReg.IsSet()) {
		return ObjectPoolInstr{};
	}

	setAsmTextDataPool(marker.Take(), offset);
	auto val = getPoolObject(app, offset, dstReg);
	return ObjectPoolInstr{ dstReg, VarItem{VarStorage::NewPool((int)offset), val} };
}

void FunctionAnalyzer::printInsnException(InsnException& e)
{
	std::cerr << "Analysis error at line " << e.location.line()
		<< " `" << e.location.function_name() << "`: " << e.cond << '\n';
	const uint64_t fn_addr = fnInfo->dartFn.Address();

	auto ins = e.insn;
	for (int i = 0; ins->address > fn_addr && i < 4; i++) {
		--ins;
	}
	while (ins != e.insn) {
		std::cerr << std::format("    {:#x}: {} {}\n", ins->address, &ins->mnemonic[0], &ins->op_str[0]);
		++ins;
	}
	std::cerr << std::format("  * {:#x}: {} {}\n", ins->address, &ins->mnemonic[0], &ins->op_str[0]);
	if (ins->address + ins->size < fnInfo->dartFn.AddressEnd()) {
		++ins;
		std::cerr << std::format("    {:#x}: {} {}\n", ins->address, &ins->mnemonic[0], &ins->op_str[0]);
	}
}

std::unique_ptr<EnterFrameInstr> FunctionAnalyzer::processEnterFrameInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_STP && insn.ops(0).reg == CSREG_DART_FP && insn.ops(1).reg == ARM64_REG_LR && insn.ops(2).mem.base == CSREG_DART_SP) {
		INSN_ASSERT(insn.writeback());
		const auto ins0_addr = insn.address();
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_MOV);
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_FP);
		INSN_ASSERT(insn.ops(1).reg == CSREG_DART_SP);
		++insn;

		fnInfo->useFramePointer = true;
		return std::make_unique<EnterFrameInstr>(insn.Wrap(ins0_addr));
	}
	return nullptr;
}

std::unique_ptr<LeaveFrameInstr> FunctionAnalyzer::processLeaveFrameInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_MOV && insn.ops(0).reg == CSREG_DART_SP && insn.ops(1).reg == CSREG_DART_FP) {
		INSN_ASSERT(fnInfo->useFramePointer);
		const auto ins0_addr = insn.address();
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_LDP && insn.op_count() == 4);
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_FP);
		INSN_ASSERT(insn.ops(1).reg == ARM64_REG_LR);
		INSN_ASSERT(insn.ops(2).mem.base == CSREG_DART_SP);
		INSN_ASSERT(insn.ops(3).imm == 0x10);
		++insn;

		return std::make_unique<LeaveFrameInstr>(insn.Wrap(ins0_addr));
	}
	return nullptr;
}

std::unique_ptr<AllocateStackInstr> FunctionAnalyzer::processAllocateStackInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_SUB && insn.ops(0).reg == CSREG_DART_SP && insn.ops(1).reg == CSREG_DART_SP && insn.ops(2).type == ARM64_OP_IMM) {
		const auto stackSize = (uint32_t)insn.ops(2).imm;
		fnInfo->stackSize = stackSize;
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<AllocateStackInstr>(insn.Wrap(ins0_addr), stackSize);
	}
	return nullptr;
}

std::unique_ptr<CheckStackOverflowInstr> FunctionAnalyzer::processCheckStackOverflowInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_LDR && insn.ops(1).mem.base == CSREG_DART_THR && insn.ops(1).mem.disp == AOT_Thread_stack_limit_offset) {
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_TMP);
		const auto ins0_addr = insn.address();
		++insn;

		// cmp SP, TMP
INSN_ASSERT(insn.id() == ARM64_INS_CMP);
INSN_ASSERT(insn.ops(0).reg == CSREG_DART_SP);
INSN_ASSERT(insn.ops(1).reg == CSREG_DART_TMP);
++insn;

INSN_ASSERT(insn.id() == ARM64_INS_B);
INSN_ASSERT(insn.ops(0).type == ARM64_OP_IMM);
uint64_t target = (uint64_t)insn.ops(0).imm;
const auto cc = insn.cc();
++insn;

if (cc == ARM64_CC_HI) {
	INSN_ASSERT(insn.IsBranch());
	INSN_ASSERT(insn.NextAddress() == target);
	target = (uint64_t)insn.ops(0).imm;
	++insn;
}
else {
	// b.ls #overflow_target
	INSN_ASSERT(cc == ARM64_CC_LS);
	//FATAL("unexpect branch condition for CheckStackOverflow");
}

if (target != 0) {
	// the dart compiler always put slow path at the end of function after "ret"
	INSN_ASSERT(target < dartFn->AddressEnd() && target >= insn.address());
	if (fnInfo->firstCheckStackOverflowAddr == 0)
		fnInfo->firstCheckStackOverflowAddr = ins0_addr;
	return std::make_unique<CheckStackOverflowInstr>(insn.Wrap(ins0_addr), target);
}
	}

	return nullptr;
}

std::unique_ptr<CallLeafRuntimeInstr> FunctionAnalyzer::processCallLeafRuntime(AsmIterator& insn)
{
	// CCallInstr::EmitNativeCode()
	if (insn.id() == ARM64_INS_AND && insn.ops(0).reg == CSREG_DART_SP && insn.ops(1).reg == CSREG_DART_SP && insn.ops(2).imm == 0xfffffffffffffff0) {
		// previous IL must be EnterFrame
		//INSN_ASSERT(fnInfo->LastIL()->Kind() == ILInstr::EnterFrame);
		// TODO: THR::call_native_through_safepoint_entry_point case and merge AllocStack IL to EnterFrame IL
		if (fnInfo->LastIL()->Kind() == ILInstr::EnterFrame) {
			InsnMarker marker(insn);
			++insn;
			if (insn.id() == ARM64_INS_MOV && insn.ops(0).reg == ARM64_REG_SP && insn.ops(1).reg == CSREG_DART_SP) {
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_LDR);
				INSN_ASSERT(insn.ops(0).reg == CSREG_DART_TMP);
				INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_THR);
				const auto thr_offset = insn.ops(1).mem.disp;
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_STR);
				INSN_ASSERT(insn.ops(0).reg == CSREG_DART_TMP);
				INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_THR && insn.ops(1).mem.disp == AOT_Thread_vm_tag_offset);
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_BLR);
				INSN_ASSERT(insn.ops(0).reg == CSREG_DART_TMP);
				++insn;

				INSN_ASSERT(insn.IsMovz());
				INSN_ASSERT(insn.ops(0).reg == CSREG_DART_TMP);
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_STR);
				INSN_ASSERT(insn.ops(0).reg == CSREG_DART_TMP);
				INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_THR && insn.ops(1).mem.disp == AOT_Thread_vm_tag_offset);
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_LDR);
				INSN_ASSERT(insn.ops(0).reg == CSREG_DART_TMP);
				INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_THR && insn.ops(1).mem.disp == AOT_Thread_saved_stack_limit_offset);
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_SUB);
				INSN_ASSERT(insn.ops(0).reg == ARM64_REG_SP);
				INSN_ASSERT(insn.ops(1).reg == CSREG_DART_TMP);
				INSN_ASSERT(insn.ops(2).imm == 1 && insn.ops(2).shift.type == ARM64_SFT_LSL && insn.ops(2).shift.value == 12);
				++insn;

				// leave frame
				auto il = processLeaveFrameInstr(insn);
				INSN_ASSERT(il);

				INSN_ASSERT(GetThreadLeafFunction(thr_offset));

				// remove previous EnterFrame
				fnInfo->RemoveLastIL();

				return std::make_unique<CallLeafRuntimeInstr>(insn.Wrap(marker.Take()), thr_offset);
			}
		}
	}
	// weird case
	// it should be easier to detect THR register if varaible tracking is fully implemented
	else if ((insn.id() == ARM64_INS_MOV && insn.ops(1).reg == CSREG_DART_THR) || 
		(insn.id() == ARM64_INS_LDR && GetThreadLeafFunction(insn.ops(1).mem.disp) && insn.ops(1).mem.base != CSREG_DART_PP && insn.ops(1).mem.disp > dart::Thread::AllocateHandle_entry_point_offset()))
	{
		InsnMarker marker(insn);

		if (insn.id() == ARM64_INS_MOV) {
			const auto tmp_reg = insn.ops(0).reg;
			++insn;

			if (!(insn.id() == ARM64_INS_LDR && insn.ops(1).mem.base == tmp_reg)) {
				return nullptr;
			}
		}

		// LDR instruction
		const auto thr_offset = insn.ops(1).mem.disp;
		const A64::Register tmp_target_reg = insn.ops(0).reg;
		++insn;

		INSN_ASSERT(GetThreadLeafFunction(thr_offset));

		std::vector<std::unique_ptr<MoveRegInstr>> movILs;
		while (true) {
			auto il = processMoveRegInstr(insn);
			INSN_ASSERT(il);
			if (il->srcReg == A64::Register::FP) {
				INSN_ASSERT(il->dstReg == A64::Register::TMP2);
				break;
			}
			else if (il->srcReg == tmp_target_reg) {
				INSN_ASSERT(il->dstReg == A64::Register::R9);
				//const auto call_target_reg = insn.ops(0).reg;
			}
			else {
				// moving for setting up call paramaeters
				movILs.push_back(std::move(il));
			}
		}
		const auto call_target_reg = ARM64_REG_X9;

		// save fp to stack
		INSN_ASSERT(insn.id() == ARM64_INS_STR && insn.writeback());
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_FP);
		INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_SP && insn.ops(1).mem.disp == -8);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_MOV);
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_FP);
		INSN_ASSERT(insn.ops(1).reg == CSREG_DART_SP);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_AND);
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_SP);
		INSN_ASSERT(insn.ops(1).reg == CSREG_DART_SP);
		INSN_ASSERT(insn.ops(2).imm == 0xfffffffffffffff0);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_MOV);
		INSN_ASSERT(insn.ops(1).reg == ARM64_REG_SP);
		const auto saved_csp_reg = insn.ops(0).reg;
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_MOV);
		INSN_ASSERT(insn.ops(0).reg == ARM64_REG_SP);
		INSN_ASSERT(insn.ops(1).reg == CSREG_DART_SP);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_BLR);
		INSN_ASSERT(insn.ops(0).reg == call_target_reg);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_MOV);
		INSN_ASSERT(insn.ops(0).reg == ARM64_REG_SP);
		INSN_ASSERT(insn.ops(1).reg == saved_csp_reg);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_MOV);
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_SP);
		INSN_ASSERT(insn.ops(1).reg == CSREG_DART_FP);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_LDR && insn.writeback());
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_FP);
		INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_SP && insn.ops(2).imm == 8);
		++insn;

		return std::make_unique<CallLeafRuntimeInstr>(insn.Wrap(marker.Take()), thr_offset, std::move(movILs));
	}

	return nullptr;
}

StoreLocalResult FunctionAnalyzer::handleStoreLocal(AsmIterator& insn, arm64_reg expected_src_reg)
{
	arm64_reg srcReg = ARM64_REG_INVALID;
	int offset = 0;
	auto saved_ins = insn.Current();

	const auto strWithRegOffset = [&] {
		const auto tmpReg = insn.ops(0).reg;
		++insn;
		if (insn.id() == ARM64_INS_STR && insn.ops(1).mem.base == CSREG_DART_FP && insn.ops(1).mem.index == tmpReg &&
			(expected_src_reg == ARM64_REG_INVALID || expected_src_reg == insn.ops(0).reg))
		{
			srcReg = insn.ops(0).reg;
			++insn;
		}
		else {
			offset = 0;
		}
	};

	if (fnInfo->stackSize > 0x100 && insn.id() == ARM64_INS_MOVN) {
		offset = insn.ops(1).imm << insn.ops(1).shift.value;
		offset = ~offset;
		// Dart use movn for setting offset only if the offset is less than -0x100
		if (offset < -0x100)
			strWithRegOffset();
	}
	else if (fnInfo->stackSize > 0x200 && insn.id() == ARM64_INS_ORR && insn.ops(1).reg == ARM64_REG_XZR && insn.ops(2).type == ARM64_OP_IMM) {
		offset = insn.ops(2).imm;
		if (offset <= 0x200)
			strWithRegOffset();
	}
	else if (insn.id() == ARM64_INS_STUR && insn.ops(1).mem.base == CSREG_DART_FP && insn.ops(1).mem.disp < 0 &&
		(expected_src_reg == ARM64_REG_INVALID || expected_src_reg == insn.ops(0).reg))
	{
		srcReg = insn.ops(0).reg;
		offset = insn.ops(1).mem.disp;
		++insn;
	}

	if (offset == 0) {
		insn.SetCurrent(saved_ins);
	}
	return StoreLocalResult{ srcReg, offset };
}

void FunctionAnalyzer::handlePrologue(AsmIterator& insn, uint64_t endPrologueAddr)
{
	{
		auto ilEnter = processEnterFrameInstr(insn);
		if (!ilEnter) {
			// no EnterFrame at a beginning of function. likely to be leaf function.
			// TODO:
			return;
		}
		fnInfo->AddIL(std::move(ilEnter));
	}

	{
		auto ilAlloc = processAllocateStackInstr(insn);
		if (!ilAlloc) {
			// no local variable (very rare)
			return;
		}
		fnInfo->AddIL(std::move(ilAlloc));
	}

	bool hasPrologue = false;
#ifdef HAS_INIT_ASYNC
	if (fnInfo->stackSize) {
		// prologue for processing function arguments
		fnInfo->InitVars();
		fnInfo->InitState();
		try {
			auto il = processPrologueParametersInstr(insn, endPrologueAddr);
			if (il) {
				fnInfo->AddIL(std::move(il));
				hasPrologue = true;
				for (auto& pending_il : fnInfo->Vars()->pending_ils) {
					fnInfo->AddIL(std::move(pending_il));
				}
				fnInfo->Vars()->pending_ils.clear();
			}
		}
		catch (InsnException& e) {
			printInsnException(e);
		}
		fnInfo->DestroyState();
		fnInfo->DestroyVars();
	}
#endif

	// below check is very useful for checking analyzing prologue because it is correct in most case
	if (hasPrologue && endPrologueAddr != 0 && endPrologueAddr != insn.address()) {
		//std::cerr << std::format("endPrologueAddr != insn.address(), {:#x} != {:#x}\n", endPrologueAddr, insn.address());
	}

	// Dart always check stack overflow if allocating stack instruction is emitted
	auto ilStack = processCheckStackOverflowInstr(insn);
	if (ilStack) {
		fnInfo->AddIL(std::move(ilStack));
	}
}

enum FunctionVarTypeId : int32_t {
	// for copying named parameters
	VtNameBegin = -100,
	VtNameParamCnt,
	VtNameParamName,
	//VtNameCurrParamPos,
	VtNameCurrParamPosSmi,
	VtNameCurrParamOffset,
	vtNameArgIdx,
	VtNameEnd,
};

std::tuple<A64::Register, A64::Register> FunctionAnalyzer::unboxParam(AsmIterator& insn, A64::Register expectedSrcReg)
{
	A64::Register srcReg;
	A64::Register dstReg;

	//dart::Double::value_offset();
	if (insn.id() == ARM64_INS_LDUR && insn.ops(1).mem.disp == AOT_Double_value_offset - dart::kHeapObjectTag) {
		// extract value from Double object
		dstReg = A64::Register{ insn.ops(0).reg };
		if (!dstReg.IsDecimal())
			return { A64::Register{}, A64::Register{} };
		srcReg = A64::Register{ insn.ops(1).mem.base };
		if (expectedSrcReg.IsSet() && expectedSrcReg != srcReg)
			return { A64::Register{}, A64::Register{} };
		++insn;
	}
	else {
		auto* saved_ins = insn.Current();
		auto il = processLoadInt32FromBoxOrSmiInstrFromSrcReg(insn, expectedSrcReg);
		if (!il)
			return { A64::Register{}, A64::Register{} };
		// unbox from srcReg to dstReg
		srcReg = il->srcObjReg;
		dstReg = il->dstReg;
	}

	return { dstReg, srcReg };
}

void FunctionAnalyzer::handleFixedParameters(AsmIterator& insn, arm64_reg paramCntReg, int paramCnt)
{
	if (paramCnt == 0)
		paramCnt = INT_MAX;
	// to get the positional parameter, the pointer is calculated from FP by skipping all optional parameter (with previous calculation)
	// then, load the parameter with LDR instruction with offset same as normal function parameter.
	for (auto i = 0; i < paramCnt; i++) {
		// don't know why some function has only one fixed positional param but the value is 2
		if (insn.id() != ARM64_INS_ADD)
			break;
		INSN_ASSERT(insn.ops(1).reg == CSREG_DART_FP);
		// shift only 2 because the number of parameter is Smi (tagged)
		INSN_ASSERT(ToCapstoneReg(insn.ops(2).reg) == paramCntReg && insn.ops(2).ext == ARM64_EXT_SXTW && insn.ops(2).shift.value == 2);
		const auto tmpReg = insn.ops(0).reg;
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_LDR);
		//INSN_ASSERT(insn.ops(0).reg == tmpReg); // ops[0] might be decimal
		// because the number of positional param might be wrong. offset cannot be calcualted.
		INSN_ASSERT(insn.ops(1).mem.base == tmpReg);//&& insn.ops(1).mem.disp == (posParamCnt - i - 1 + 2) * sizeof(void*)); // +2 for saved fp,lr
		const auto valReg = insn.ops(0).reg;
		++insn;

		fnInfo->State()->ClearRegister(tmpReg);
		auto val = fnInfo->Vars()->ValParam(fnInfo->params.NumParam());
		fnInfo->State()->SetRegister(valReg, val);

		// the parameter value might be saved to stack as if it is a local variable but with fixed negative offset from FP
		const auto storeRes = handleStoreLocal(insn, valReg);
		if (storeRes.fpOffset != 0) {
			fnInfo->State()->SetLocal(storeRes.fpOffset, val);
		}

		fnInfo->params.add(FnParamInfo{ valReg, storeRes.fpOffset });
	}

	fnInfo->params.numFixedParam = fnInfo->params.NumParam();
	// TODO:
	// there is a case a function cannot matching pattern. so num fixed param is 0 even it is not
	if (!dartFn->IsStatic() && fnInfo->params.numFixedParam > 0) {
		// class method. first parameter is "this"
		fnInfo->params[0].name = "this";
		fnInfo->params[0].type = dartFn->Class().DeclarationType(); // TODO: class with type arguments (generic class)
	}
}

void FunctionAnalyzer::handleOptionalPositionalParameters(AsmIterator& insn, arm64_reg optionalParamCntReg)
{
	// basically, it is a loop of checking a number of parameter and loading values
	// if num_param < 1 (cmp and b.lt)
	//   jmp to all default values
	// if num_param < 2
	//   load first param and default values
	// ...
	//   all params are passed and load them
	// jmp to storing to stack
	// load all default values
	int i = 0;
	std::vector<int64_t> missingBranchTargets;
	while (insn.id() == ARM64_INS_CMP) {
		INSN_ASSERT(ToCapstoneReg(insn.ops(0).reg) == optionalParamCntReg);
		INSN_ASSERT(insn.ops(1).imm == (i + 1) << 1);
		++insn;

		if (insn.IsBranch(ARM64_CC_GE)) {
			// special case. parameters are not used.
			const auto defaultValueTarget = insn.ops(0).imm;
			INSN_ASSERT(defaultValueTarget == insn.NextAddress());
			for (const auto missingTarget : missingBranchTargets) {
				INSN_ASSERT(missingTarget == defaultValueTarget);
			}
			missingBranchTargets.clear();
			++insn;
			break;
		}

		INSN_ASSERT(insn.IsBranch(ARM64_CC_LT));
		const auto defaultValueTarget = insn.ops(0).imm;
		missingBranchTargets.push_back(defaultValueTarget);
		++insn;

		// parameter might not be used and no loading value
		if (insn.id() == ARM64_INS_ADD && insn.ops(1).reg == CSREG_DART_FP) {
			// shift only 2 because the number of parameter is Smi (tagged)
			INSN_ASSERT(ToCapstoneReg(insn.ops(2).reg) == optionalParamCntReg && insn.ops(2).ext == ARM64_EXT_SXTW && insn.ops(2).shift.value == 2);
			const auto tmpReg = insn.ops(0).reg;
			++insn;

			// LDUR is used when offset is negative
			INSN_ASSERT(insn.id() == ARM64_INS_LDR || insn.id() == ARM64_INS_LDUR);
			//INSN_ASSERT(insn.ops(0).reg == tmpReg); // ops[0] might be decimal
			INSN_ASSERT(insn.ops(1).mem.base == tmpReg && insn.ops(1).mem.disp == 8 * (1 - i));
			const auto valReg = insn.ops(0).reg;
			fnInfo->State()->ClearRegister(tmpReg);
			auto val = fnInfo->Vars()->ValParam(fnInfo->params.NumParam());
			fnInfo->State()->SetRegister(valReg, val);
			fnInfo->params.add(FnParamInfo{ A64::Register{ valReg } });
			++insn;

			// the parameter value might be saved to stack as if it is a local variable but with fixed negative offset from FP
			const auto storeRes = handleStoreLocal(insn, valReg);
			if (storeRes.fpOffset != 0) {
				fnInfo->State()->SetLocal(storeRes.fpOffset, val);
			}
		}
		else {
			fnInfo->params.add(FnParamInfo{ A64::Register{} });
		}

		++i;
	}

	if (!missingBranchTargets.empty()) {
		// all params are passed
		while (true) {
			// might unbox parameters (int, double) to native value
			const auto& [dstReg, srcReg] = unboxParam(insn);
			if (!dstReg.IsSet())
				break;

			auto val = fnInfo->State()->MoveRegister(dstReg, srcReg);
			INSN_ASSERT(val);
			auto& param = fnInfo->params[val->AsParam()->idx];
			param.type = app.TypeDb()->Get(dstReg.IsDecimal() ? dart::kDoubleCid : app.DartIntCid());
			param.valReg = dstReg;
		}

		// might be moved to another register
		while (!insn.IsBranch()) {
			INSN_ASSERT(insn.id() == ARM64_INS_MOV);
			INSN_ASSERT(insn.ops(0).type == ARM64_OP_REG && insn.ops(1).type == ARM64_OP_REG && insn.ops(1).reg != CSREG_DART_NULL);
			const auto srcReg = A64::Register{ insn.ops(1).reg };
			//auto found = fnInfo->params.movValReg(A64::Register{ insn.ops(0).reg }, srcReg);
			//INSN_ASSERT(found);
			auto valParam = fnInfo->State()->MoveRegister(insn.ops(0).reg, srcReg);
			INSN_ASSERT(valParam);
			++insn;
		}
		const auto storingBranchTarget = insn.ops(0).imm;
		++insn;

		// skip branches that only for missing some parameters to missing all parameters
		const auto num_optional_param = missingBranchTargets.size();
		while (insn.address() != missingBranchTargets.front()) {
			++insn;
		}

		// default values (missing all parameters branch)
		// it is difficult to check dstReg without optParam2 variable because
		//   there might moving register after loading default value
		// the proper way is another state for this branch
		std::vector<FnParamInfo> optParams2;
		while (insn.address() < storingBranchTarget) {
			// might be load from PP, mov from constant, mov for moving register
			auto ilValue = processLoadValueInstr(insn);
			if (ilValue) {
				optParams2.push_back(FnParamInfo{ ilValue->dstReg, ilValue->val.TakeValue() });
			}
			else if (insn.id() == ARM64_INS_MOV) {
				// moving register
				INSN_ASSERT(insn.ops(0).type == ARM64_OP_REG && insn.ops(1).type == ARM64_OP_REG);
				const auto srcReg = A64::Register{ insn.ops(1).reg };
				auto itr = std::find_if(optParams2.begin(), optParams2.end(), [&](auto const& param) { return param.valReg == srcReg; });
				INSN_ASSERT(itr != optParams2.end());
				itr->valReg = A64::Register{ insn.ops(0).reg };
				++insn;
			}
			else {
				FATAL("unexpected instruction");
			}
		}

		INSN_ASSERT(optParams2.size() <= fnInfo->params.NumOptionalParam());
		// Note: optParams2 MUST be ordered same as previous steps but some parameter might have no default value (required or unused)
		for (int i = fnInfo->params.numFixedParam, j = 0; i < fnInfo->params.NumParam(); i++) {
			if (fnInfo->params[i].valReg.IsSet()) {
				INSN_ASSERT(fnInfo->State()->GetValue(optParams2[j].valReg)->AsParam()->idx == i);
				INSN_ASSERT(j < optParams2.size());
				fnInfo->params[i].val = std::move(optParams2[j].val);
				j++;
			}
		}

		while (true) {
			const auto storeRes = handleStoreLocal(insn);
			if (storeRes.fpOffset == 0)
				break;
			auto val = fnInfo->State()->GetValue(storeRes.srcReg);
			INSN_ASSERT(val);
			INSN_ASSERT(val && val->RawTypeId() == VarValue::Parameter);
			fnInfo->State()->SetLocal(storeRes.fpOffset, val);
		}
	}
}

void FunctionAnalyzer::handleOptionalNamedParameters(AsmIterator& insn, arm64_reg paramCntReg)
{
	// check for named parameters case
	// load the first named parameter name from ArgumentsDescriptor (generated code uses constant for access first named parameter)
	if (!(insn.id() == ARM64_INS_LDUR && fnInfo->State()->GetValue(insn.ops(1).reg) == fnInfo->Vars()->ValArgsDesc() && insn.ops(1).mem.disp >= AOT_ArgumentsDescriptor_first_named_entry_offset - dart::kHeapObjectTag))
		return;

	VarValue valNameParamCnt(VtNameParamCnt);
	VarValue valNameParamName(VtNameParamName);
	//VarValue valNameCurrParamPos(VtNameCurrParamPos);
	VarValue valNameCurrParamPosSmi(VtNameCurrParamPosSmi);
	VarValue valNameCurrParamOffset(VtNameCurrParamOffset);
	VarValue valNameArgIdx(vtNameArgIdx);

	// names of the optional parameters are alphabetically sorted
	// so the order might not be same as written code
	int nameParamCnt = 0; // count only non-required named parameter
	bool isLastName = false;

	// load named parameter value (name or position) from ArgumentsDescriptor with currParamOffset variable
	// return -1 if it is not loaded ArgumentsDescriptor
	const auto loadNamedParamValue = [&](int expectedOffset, VarValue* val) {
		// first instruction must be "add dstReg, offsetReg, #offset"
		if (insn.id() == ARM64_INS_ADD && fnInfo->State()->GetValue(insn.ops(1).reg) == &valNameCurrParamOffset) {
			const auto offset = (int)insn.ops(2).imm;
			if (offset == expectedOffset) {
				const auto tmpReg = insn.ops(0).reg;
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_ADD);
				INSN_ASSERT(fnInfo->State()->GetValue(insn.ops(1).reg) == fnInfo->Vars()->ValArgsDesc());
				INSN_ASSERT(insn.ops(2).reg == tmpReg && insn.ops(2).ext == ARM64_EXT_SXTW && insn.ops(2).shift.value == 1);
				const auto tmpReg2 = insn.ops(0).reg;
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_LDUR);
				INSN_ASSERT(insn.ops(1).mem.base == tmpReg2 && insn.ops(1).mem.disp == sizeof(void*) * 2 - dart::kHeapObjectTag);
				const auto dstReg = ToCapstoneReg(insn.ops(0).reg);
				++insn;

				handleDecompressPointer(insn, dstReg);

				fnInfo->State()->ClearRegister(tmpReg);
				fnInfo->State()->ClearRegister(tmpReg2);
				fnInfo->State()->SetRegister(dstReg, val);
			}
			return offset;
		}
		return -1;
	};

	// load named parameter value (name or position) from ArgumentsDescriptor with known offset
	// return -1 if it is not loaded ArgumentsDescriptor
	const auto loadNamedParamValueKnownOffset = [&](int expectedOffset, VarValue* val) {
		if (insn.id() == ARM64_INS_LDUR && fnInfo->State()->GetValue(insn.ops(1).reg) == fnInfo->Vars()->ValArgsDesc() &&
			insn.ops(1).mem.disp >= AOT_ArgumentsDescriptor_first_named_entry_offset - dart::kHeapObjectTag)
		{
			const auto name_offset = AOT_ArgumentsDescriptor_first_named_entry_offset +
				(AOT_ArgumentsDescriptor_named_entry_size * fnInfo->params.NumOptionalParam()) +
				AOT_ArgumentsDescriptor_name_offset - dart::kHeapObjectTag;

			int offset = insn.ops(1).mem.disp - name_offset;
			// there might be "required" named parameter but not used in a function
			// then, the Dart compiler skips the parameter loading to the next parameter
			while (offset >= AOT_ArgumentsDescriptor_named_entry_size) {
				fnInfo->params.add(FnParamInfo{ "required" });
				offset -= AOT_ArgumentsDescriptor_named_entry_size;
			}
			if (offset == expectedOffset) {
				const auto dstReg = ToCapstoneReg(insn.ops(0).reg);
				++insn;

				handleDecompressPointer(insn, dstReg);

				fnInfo->State()->SetRegister(dstReg, val);
			}

			return offset;
		}
		return -1;
	};

	// ## loop of checking name and loading value
	// curr_param_pos = 0
	// if argsDesc[curr_param_pos].name == "name1"
	//    load argument position in stack from argsDesc[curr_param_pos].pos
	//    load param value from stack (call arguments)
	//    increment curr_param_pos
	// else
	//    load default value
	// might store value into local variable
	// ... if argsDesc[curr_param_pos].name == "name2"
	// ...
	// ## for "required" argument, there is no name check. psuedo code will be
	// load argument position in stack from argsDesc[curr_param_pos].pos
	// load param value from stack (call arguments)
	// increment curr_param_pos
	// ## the first name is handled specifically because curr_param_pos is known value
	// ## the last name is a bit different too because no need for incrementing curr_param_pos

	bool isRequired = false;
	while (!isLastName) {
		// load current parameter name from ArgumentsDescriptor
		// the load code uses fixed offset of ArgumentsDescriptor if offset is known (first parameter), 
		// if the parameter is "required", no parameter name comparison and also no default value branch
		if (nameParamCnt) {
			// load from currParamPosReg
			// 0x4128dc: lsl  x5, x2, #1            ; make Smi
			// 0x4128e0: lsl  w6, w5, #1            ; named pair index
			// 0x4128e4: add  w7, w6, #8            ; add 4 (number of values before starting of named parameters)
			// 0x4128e8: add  x16, x0, w7, sxtw #1  ; Smi to array offset
			// 0x4128ec: ldur  w8, [x16, #0xf]      ; offset 0x10 because of TaggedObject and 2 headers

			// if previous parameter is "required", only currParamPosSmi is used and updated. currParamPos is not used.
			if (!isRequired) {
				// make it be Smi
				INSN_ASSERT(insn.id() == ARM64_INS_LSL);
				INSN_ASSERT(fnInfo->State()->GetValue(insn.ops(1).reg) == fnInfo->Vars()->ValCurrNumNameParam());
				INSN_ASSERT(insn.ops(2).imm == 1);
				fnInfo->State()->SetRegister(insn.ops(0).reg, &valNameCurrParamPosSmi);
				++insn;
			}

			// weird case. add 1 to currParamPosSmiReg. maybe this named parameter is not used and skip it.
			if (insn.id() == ARM64_INS_ADD) {
				INSN_ASSERT(fnInfo->State()->GetValue(insn.ops(1).reg) == &valNameCurrParamPosSmi);
				INSN_ASSERT(insn.ops(2).imm == 2); // tagged value of 1
				fnInfo->State()->MoveRegister(insn.ops(0).reg, insn.ops(1).reg);
				++insn;
			}

			// calculate the array index of current named parameter (multiply by 2 because of name and index pair)
			INSN_ASSERT(insn.id() == ARM64_INS_LSL);
			INSN_ASSERT(fnInfo->State()->GetValue(insn.ops(1).reg) == &valNameCurrParamPosSmi);
			INSN_ASSERT(insn.ops(2).imm == 1);
			fnInfo->State()->SetRegister(insn.ops(0).reg, &valNameCurrParamOffset);
			++insn;

			// calculate offset in ArgumentsDescriptor and load it (0x8 is for parameter name and 0xa is for parameter value index)
			// if a parameter is required, this instruction is for loading parameter value index
			const auto nameOffset = loadNamedParamValue(8, &valNameParamName);
			if (nameOffset == 8)
				isRequired = false;
			else if (nameOffset == 0xa)
				isRequired = true;
			else
				INSN_ASSERT(false);
		}
		else {
			// "required" might be at the first parameter
			const auto nameOffset = loadNamedParamValueKnownOffset(AOT_ArgumentsDescriptor_name_offset, &valNameParamName);
			if (nameOffset == AOT_ArgumentsDescriptor_name_offset) {
				// known offset. first non "required" name
				isRequired = false;
			}
			else if (nameOffset == AOT_ArgumentsDescriptor_position_offset) {
				isRequired = true;
			}
			else {
				INSN_ASSERT(nameOffset == -1);
				INSN_ASSERT(fnInfo->params.NumOptionalParam() > 0);
				break;
			}
		}

		// if named parameter is "required", no fetching name for comparison
		// compare the parameter name
		std::string paramName;
		int64_t nameMismatchAddr;
		if (!isRequired) {
			// get expected parameter name from PP
			const auto objPoolInstr = getObjectPoolInstruction(insn);
			INSN_ASSERT(objPoolInstr.dstReg == A64::TMP_REG);
			INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kStringCid);
			paramName = objPoolInstr.item.Get<VarString>()->str;

			INSN_ASSERT(insn.id() == ARM64_INS_CMP);
			INSN_ASSERT(fnInfo->State()->GetValue(insn.ops(0).reg) == &valNameParamName && A64::Register{ insn.ops(1).reg } == objPoolInstr.dstReg);
			++insn;

			if (insn.IsBranch(ARM64_CC_EQ)) {
				// last named parameter and not used
				const auto branchTarget = insn.ops(0).imm;
				++insn;
				INSN_ASSERT(branchTarget == insn.address());
				fnInfo->params.add(FnParamInfo{ std::move(paramName) });
				++nameParamCnt;
				break;
			}

			INSN_ASSERT(insn.IsBranch(ARM64_CC_NE));
			nameMismatchAddr = insn.ops(0).imm;
			++insn;
		}
		else {
			paramName = "required"; // TODO: should be flag for "required" attribute
			nameMismatchAddr = 0;
		}

		// matched parameter name branch. load the value from stack.
		// Note: some parameter is not loaded at all (maybe not used)
		auto doLoadValue = false;
		if (nameParamCnt) {
			// 0x412904: add  w2, w6, #0xa
			// 0x412908: add  x16, x0, w2, sxtw #1
			// 0x41290c: ldur  w6, [x16, #0xf]
			const auto posOffset = loadNamedParamValue(0xa, &valNameArgIdx);
			if (posOffset == 0xa)
				doLoadValue = true;
			else
				INSN_ASSERT(posOffset == -1); // must not be load instruction
		}
		else {
			const auto posOffset = loadNamedParamValueKnownOffset(AOT_ArgumentsDescriptor_position_offset, &valNameArgIdx);
			if (posOffset == AOT_ArgumentsDescriptor_position_offset)
				doLoadValue = true;
			else
				INSN_ASSERT(posOffset == -1); // must not be load instruction
		}

		// loading a function argument from stack with argIdxReg
		if (doLoadValue) {
			// fetch name parameter value
			// 0x4128bc: sub  w4, w1, w2  ; total_param - args_idx
			// 0x4128c0: add  x2, x29, w4, sxtw #2
			// 0x4128c4: ldr  x2, [x2, #8]
			INSN_ASSERT(insn.id() == ARM64_INS_SUB);
			INSN_ASSERT(ToCapstoneReg(insn.ops(1).reg) == paramCntReg);
			INSN_ASSERT(fnInfo->State()->GetValue(insn.ops(2).reg) == &valNameArgIdx);
			const auto tmpReg = insn.ops(0).reg;
			++insn;

			INSN_ASSERT(insn.id() == ARM64_INS_ADD);
			INSN_ASSERT(insn.ops(1).reg == CSREG_DART_FP);
			INSN_ASSERT(insn.ops(2).reg == tmpReg && insn.ops(2).ext == ARM64_EXT_SXTW && insn.ops(2).shift.value == 2);
			const auto tmpReg2 = insn.ops(0).reg;
			++insn;

			INSN_ASSERT(insn.id() == ARM64_INS_LDR);
			INSN_ASSERT(insn.ops(0).reg == tmpReg2);
			INSN_ASSERT(insn.ops(1).mem.base == tmpReg2 && insn.ops(1).mem.disp == sizeof(void*));
			const auto valReg = insn.ops(0).reg;
			++insn;

			fnInfo->State()->ClearRegister(tmpReg);
			fnInfo->State()->ClearRegister(tmpReg2);
			auto val = fnInfo->Vars()->ValParam(fnInfo->params.NumParam());
			fnInfo->State()->SetRegister(valReg, val);
			fnInfo->params.add(FnParamInfo{ A64::Register{valReg}, std::move(paramName) });

			const auto storeRes = handleStoreLocal(insn, valReg);
			if (storeRes.fpOffset != 0)
				fnInfo->State()->SetLocal(storeRes.fpOffset, val);
		}
		else {
			fnInfo->params.add(FnParamInfo{ std::move(paramName) });
		}

		// for the first parameter name, this will be set with MOVZ (after unboxing)
		if (nameParamCnt) {
			// if this is not last named parameter, increment the param position (smi)
			if (insn.id() == ARM64_INS_ADD) {
				// 0x412920: add  w2, w5, #2
				INSN_ASSERT(fnInfo->State()->GetValue(insn.ops(1).reg) == &valNameCurrParamPosSmi);
				INSN_ASSERT(insn.ops(2).imm == 2);
				fnInfo->State()->MoveRegister(insn.ops(0).reg, insn.ops(1).reg);
				++insn;
			}
			else {
				isLastName = true;
			}
		}

		// unbox param for int and double
		if (doLoadValue) {
			const auto& [dstReg, srcReg] = unboxParam(insn, fnInfo->params.back().valReg);
			if (dstReg.IsSet()) {
				const auto val = fnInfo->State()->MoveRegister(dstReg, srcReg);
				INSN_ASSERT(val && val->AsParam()->idx == fnInfo->params.NumParam() - 1);
				fnInfo->params.back().type = app.TypeDb()->Get(dstReg.IsDecimal() ? dart::kDoubleCid : app.DartIntCid());
				fnInfo->params.back().valReg = dstReg;
			}
		}

		if (!isRequired) {
			// Smi to native. only non first and last name do it
			if (nameParamCnt && !isLastName) {
				// 0x412924: sbfx  x5, x2, #1, #0x1f
				if (insn.id() == ARM64_INS_SBFX) {
					INSN_ASSERT(fnInfo->State()->GetValue(insn.ops(1).reg) == &valNameCurrParamPosSmi);
					INSN_ASSERT(insn.ops(2).imm == 1 && insn.ops(3).imm == 0x1f);
					fnInfo->State()->SetRegister(insn.ops(0).reg, fnInfo->Vars()->ValCurrNumNameParam());
					++insn;
				}
			}
		}

		// doing loop here because the moving value to another register can be in any order and multiple time
		while (insn.id() == ARM64_INS_MOV && insn.ops(1).type == ARM64_OP_REG && insn.ops(1).reg != CSREG_DART_NULL) {
			auto val = fnInfo->State()->MoveRegister(insn.ops(0).reg, insn.ops(1).reg);
			INSN_ASSERT(val);
			++insn;
		}

		// first non "required" parameter, set the parameter position value
		if (!nameParamCnt && !isRequired) {
			if (insn.IsMovz() && insn.ops(1).imm == fnInfo->params.NumOptionalParam()) {
				fnInfo->State()->SetRegister(insn.ops(0).reg, fnInfo->Vars()->ValCurrNumNameParam());
				++insn;
			}
			else {
				isLastName = true;
			}
		}

		if (!isRequired) {
			// branch for skipping loading default value
			// no skipping if no loading parameter value
			const auto nextParamAddr = [&] {
				if (doLoadValue || nameParamCnt == 0) {
					INSN_ASSERT(insn.IsBranch());
					const auto nextParamAddr = insn.ops(0).imm;
					++insn;
					return nextParamAddr;
				}
				return (int64_t)0;
			}();

			if (nextParamAddr) {
				if (nameMismatchAddr == nextParamAddr) {
					// assume dead code
					while (insn.address() < nextParamAddr)
						++insn;
					break;
				}
			}

			// TODO: split state for match and mismatch branch, so all param can be tracked correctly
			if (insn.id() == ARM64_INS_SBFX && insn.ops(2).imm == 1 && insn.ops(3).imm == 0x1f) {
				// assume curr param pos Smi to native in default branch
				++insn;
			}

			while (insn.id() == ARM64_INS_MOV && insn.ops(1).type == ARM64_OP_REG && insn.ops(1).reg != CSREG_DART_NULL) {
				// don't move register on another branch
				if (nextParamAddr == 0) {
					auto val = fnInfo->State()->MoveRegister(insn.ops(0).reg, insn.ops(1).reg);
					INSN_ASSERT(val);
				}
				// TODO: verify final register for valNameCurrParamPos (set when "nameParamCnt && !isLastName") in this branch 
				//         is same as another branch

				++insn;
			}

			// assign param position in case of first non "required" name in default branch (no passing this parameter)
			const auto processAssignParamPos0 = [&] {
				if (insn.IsMovz() && fnInfo->State()->GetValue(insn.ops(0).reg) == fnInfo->Vars()->ValCurrNumNameParam() && insn.ops(1).imm == fnInfo->params.NumOptionalParam() - 1) {
					++insn;
					return true;
				}
				return false;
			};

			auto foundAssignParamPos0 = false;
			if (!nameParamCnt && !isLastName) {
				foundAssignParamPos0 = processAssignParamPos0();
			}

			// load default value and setting paramPos can be any order
			if (doLoadValue) {
				// load default value
				auto il = processLoadValueInstr(insn);
				if (!il)
					break;
				INSN_ASSERT(fnInfo->State()->GetValue(il->dstReg)->AsParam()->idx == fnInfo->params.NumParam() - 1);
				fnInfo->params.back().val = il->val.TakeValue();

				// very weird case in Dart 3.2.3. duplicate loading value
				if (nextParamAddr != 0 && insn.address() < nextParamAddr) {
					auto min_diff = (!nameParamCnt && !isLastName && !foundAssignParamPos0) ? 4 : 0;
					if (nextParamAddr - insn.address() > min_diff)
						processLoadValueInstr(insn);
				}
			}

			if (!nameParamCnt && !isLastName && !foundAssignParamPos0) {
				foundAssignParamPos0 = processAssignParamPos0();
			}

			INSN_ASSERT(nextParamAddr == 0 || insn.address() == nextParamAddr);
		}

		if (doLoadValue) {
			// this value might be stored into a local variable
			const auto storeRes = handleStoreLocal(insn);
			if (storeRes.fpOffset != 0) {
				auto val = fnInfo->State()->GetValue(storeRes.srcReg);
				INSN_ASSERT(val && val->RawTypeId() == VarValue::Parameter);
				fnInfo->State()->SetLocal(storeRes.fpOffset, val);
			}
		}
		// end of a named parameter
		// count only non-required named parameter
		if (!isRequired)
			++nameParamCnt;

		// extra check for end of named parameters
		if (nameParamCnt) {
			if (insn.id() != ARM64_INS_LSL) {
				break;
			}
		}
	}

	fnInfo->params.isNamedParam = true;
	// remove all reference to temporary variables for loading named parameters
	for (auto& reg : fnInfo->State()->regs) {
		if (reg && reg->RawTypeId() > VtNameBegin && reg->RawTypeId() < VtNameEnd)
			reg = nullptr;
	}
}

void FunctionAnalyzer::handleArgumentsDescriptorTypeArguments(AsmIterator& insn)
{
	if (!(insn.id() == ARM64_INS_LDUR && fnInfo->State()->GetValue(insn.ops(1).reg) == fnInfo->Vars()->ValArgsDesc() && insn.ops(1).mem.disp == AOT_ArgumentsDescriptor_type_args_len_offset - dart::kHeapObjectTag))
		return;

	const auto typeArgLenReg = ToCapstoneReg(insn.ops(0).reg);
	++insn;

	handleDecompressPointer(insn, typeArgLenReg);

	const auto storeTypeArgLenRes = handleStoreLocal(insn, typeArgLenReg);
	if (storeTypeArgLenRes.fpOffset != 0) {
		// TODO: save type argument length to local variable
		//storeTypeArgLenRes.fpOffset;
	}

	if (insn.id() == ARM64_INS_CBZ) {
		// same as nop (why compiler does not remove it)
		INSN_ASSERT(ToCapstoneReg(insn.ops(0).reg) == typeArgLenReg);
		const auto contAddr = insn.ops(1).imm;
		++insn;

		INSN_ASSERT(insn.address() == contAddr);
		return;
	}

	// if (typeArgLen == 0)
	INSN_ASSERT(insn.id() == ARM64_INS_CBNZ);
	INSN_ASSERT(ToCapstoneReg(insn.ops(0).reg) == typeArgLenReg);
	const auto loadTypeArgAddr = insn.ops(1).imm;
	++insn;

	//   typeArg = null
	INSN_ASSERT(insn.id() == ARM64_INS_MOV);
	INSN_ASSERT(insn.ops(1).reg == CSREG_DART_NULL);
	const auto typeArgReg = insn.ops(0).reg;
	++insn;

	INSN_ASSERT(insn.id() == ARM64_INS_B);
	const auto contAddr = insn.ops(0).reg;
	++insn;

	// else
	//   typeArgPos = ArgsDesc->typeArg
	INSN_ASSERT(insn.address() == loadTypeArgAddr);
	INSN_ASSERT(insn.id() == ARM64_INS_LDUR);
	INSN_ASSERT(fnInfo->State()->GetValue(insn.ops(1).reg) == fnInfo->Vars()->ValArgsDesc() && insn.ops(1).mem.disp == AOT_ArgumentsDescriptor_size_offset - dart::kHeapObjectTag);
	const auto sizeReg = ToCapstoneReg(insn.ops(0).reg);
	fnInfo->State()->ClearRegister(sizeReg);
	++insn;

	handleDecompressPointer(insn, sizeReg);

	INSN_ASSERT(insn.id() == ARM64_INS_ADD);
	INSN_ASSERT(insn.ops(1).reg == CSREG_DART_FP);
	INSN_ASSERT(ToCapstoneReg(insn.ops(2).reg) == sizeReg && insn.ops(2).ext == ARM64_EXT_SXTW && insn.ops(2).shift.value == 2);
	const auto tmpReg = insn.ops(0).reg;
	fnInfo->State()->ClearRegister(tmpReg);
	++insn;

	INSN_ASSERT(insn.id() == ARM64_INS_LDR);
	INSN_ASSERT(insn.ops(1).mem.base == tmpReg && insn.ops(1).mem.disp == 0x10);
	INSN_ASSERT(insn.ops(0).reg == tmpReg);
	++insn;

	if (tmpReg != typeArgReg) {
		INSN_ASSERT(insn.id() == ARM64_INS_MOV);
		INSN_ASSERT(insn.ops(0).reg == typeArgReg);
		INSN_ASSERT(insn.ops(1).reg == tmpReg);
		++insn;
	}
	fnInfo->typeArgumentReg = typeArgReg;
	// as of now, track only function parameters, so clear the type argument register as if it is freed
	fnInfo->State()->ClearRegister(fnInfo->typeArgumentReg);

	INSN_ASSERT(insn.address() == contAddr);
	if (insn.id() == ARM64_INS_CBNZ && ToCapstoneReg(insn.ops(0).reg) == typeArgLenReg) {
		// function also contains type parameters with "extends"
		//   if (typeArgLen == 0) typeArg_final = from_PP
		//   else typeArg_final = typeArg
		const auto elseAddr = insn.ops(1).imm;
		++insn;

		const auto objPoolInstr = getObjectPoolInstruction(insn);
		INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kTypeArgumentsCid);
		// TODO: make this function type parameter to extends from this value
		//objPoolInstr.item.Get<VarTypeArgument>();
		fnInfo->State()->ClearRegister(objPoolInstr.dstReg);

		INSN_ASSERT(insn.IsBranch());
		const auto contAddr = insn.ops(0).imm;
		++insn;

		INSN_ASSERT(elseAddr == insn.address());
		INSN_ASSERT(insn.id() == ARM64_INS_MOV);
		INSN_ASSERT(insn.ops(1).reg == typeArgReg);
		const auto finalTypeArgReg = A64::Register{ insn.ops(0).reg };
		INSN_ASSERT(finalTypeArgReg == objPoolInstr.dstReg);
		++insn;

		INSN_ASSERT(contAddr == insn.address());
		fnInfo->typeArgumentReg = finalTypeArgReg;
	}

	// as of now, track only function parameters, so clear the type argument register as if it is freed
	fnInfo->State()->ClearRegister(fnInfo->typeArgumentReg);
}

std::unique_ptr<SetupParametersInstr> FunctionAnalyzer::processPrologueParametersInstr(AsmIterator& insn, uint64_t endPrologueAddr)
{
	// reversing of PrologueBuilder::BuildPrologue() which compose of 3 functions
	// - PrologueBuilder::BuildParameterHandling()
	//   - fixed parameters
	//   - optional parameters (positional or named)
	// - PrologueBuilder::BuildClosureContextHandling()
	// - PrologueBuilder::BuildTypeArgumentsHandling()
	// this function is only called at prologue state
	// some variable initialization might be inserted into between prologue instructions.
	//   these ILs will be appended after Prologue IL even the assembly is before the prologue ends
	// Note: with method extractors optmization (since Dart 3.4), arguments might pass via registers (R1, R2, R3).
	//   now, only see in closure and async method.
	auto optionalParamCntReg = ARM64_REG_INVALID;
	auto firstParamReg = ARM64_REG_INVALID;
	InsnMarker marker(insn);

	const auto handleInitialization = [&] {
		while (true) {
			auto il = processLoadValueInstr(insn);
			if (!il)
				break;
			fnInfo->State()->SetRegister(il->dstReg, il->val.Value());
			fnInfo->Vars()->pending_ils.push_back(std::move(il));
		}
	};

	// initialize synthetic :suspend_state
	// it might be async or sync* function e.g. https://github.com/flutter/flutter/blob/3.16.5/packages/flutter/lib/src/widgets/overlay.dart#L305
	// ref: compiler/backend/flow_graph_compiler_arm64.cc - FlowGraphCompiler::EmitPrologue()
	const bool needSuspendState = [&] {
		if (insn.id() == ARM64_INS_STUR && insn.ops(0).reg == CSREG_DART_NULL && insn.ops(1).mem.base == CSREG_DART_FP && insn.ops(1).mem.disp == -8) {
			++insn;
			return true;
		}
		return false;
	}();

	const auto mightBeFirstParamOffset = [&](int offset) {
		if (offset <= sizeof(void*))
			return false;
		if (dartFn->NumParam() != 0) {
			if (offset != dartFn->FirstParamOffset())
				return false;
		}
		else if (offset != fnInfo->asmTexts.MaxParamStackOffset())
			return false;
		return true;
	};

	if (needSuspendState || dartFn->IsAsync()) {
		if (dartFn->IsAsync())
			INSN_ASSERT(needSuspendState);
		else
			; // TODO: it is sync* function

		// very rare case, initialization here
		if (insn.id() == ARM64_INS_MOV && insn.ops(1).reg == CSREG_DART_NULL) {
			auto item = VarItem{ VarStorage::Immediate, new VarNull() };
			fnInfo->Vars()->pending_ils.push_back(std::make_unique<LoadValueInstr>(AddrRange(insn.address(), insn.NextAddress()), A64::Register{insn.ops(0).reg}, std::move(item)));
			++insn;
		}

		// saving function parameters is needed only for async function or optional parameters
		// When there is an optional parameter, ArgumentDescriptor (r4) is always used for determining the number of optional parameters by
		//   number_of_optional_params = ArgumentsDescriptor.count - number_of_fixed_params
		// In case of async without optional parameters, the number of optional parameters is set by mov instruction
		//   number_of_optional_params (r0) = 0
		// but async function prologue might use ArgumentsDescriptor when a TypeArgument is needed
		if (insn.IsMovz() && insn.ops(1).imm == 0) {
			// async function without optional parameters
			optionalParamCntReg = insn.ops(0).reg;
			++insn;
		}
		// async function returns Future<R> template
		//   there is also a case when setting fixed param count to 0 and using ArgumentsDescriptor (mov x1, x4)
		//   e.g. package:flutter/src/foundation/_isolates_io.dart (compute function)
		//   e.g. package:flutter/src/services/platform_channel.dart (compute MethodChannel::_invokeMethod)
	}
	else if (dartFn->IsClosure()) {
		handleInitialization();

		// if no saving function arguments (no optional parameter), a first argument MUST be loaded first.
		if (insn.id() == ARM64_INS_LDR && insn.ops(1).mem.base == CSREG_DART_FP && insn.ops(1).mem.disp > 0) {
			const auto firstParamOffset = insn.ops(1).mem.disp;
			// function might have multiple parameter but the function uses only last parameter
			// so, below detection might be wrong
			if (!mightBeFirstParamOffset(firstParamOffset))
				return nullptr;
			firstParamReg = insn.ops(0).reg;
			// Note: this might not be first function argument
			fnInfo->State()->SetRegister(A64::Register{ insn.ops(0).reg }, fnInfo->Vars()->ValParam(0));
			//const int numParam = ((firstParamOffset - 0x10) / sizeof(void*)) + 1;
			++insn;
		}
	}

	// ArgumentsDescriptor is Dart array. Memory Layout
	// - TaggedObject (8 bytes)
	// - Array Type Arguments (Compress pointer 4 bytes), Array Length (Smi 4 bytes)
	// - ArgumentsDescriptor
	//   - type arguments length (Smi 4 bytes)
	//   - count (Smi 4 bytes)
	//   - size (Smi 4 bytes)
	//   - positional count (Smi 4 bytes)
	//   - first named param (String and argument index)
	// load a number of parameters from ArgumentDescriptor (X0) to first free register (X1)
	const auto argsDescReg = [&] {
		auto argsDescReg = ARM64_REG_INVALID;
		if (fnInfo->State()->GetValue(CSREG_ARGS_DESC) == nullptr) {
			const auto storeArgsDescRes = handleStoreLocal(insn, CSREG_ARGS_DESC);
			if (storeArgsDescRes.fpOffset != 0) {
				// save ArgumentsDescriptor to stack
				fnInfo->State()->SetLocal(storeArgsDescRes.fpOffset, fnInfo->Vars()->ValArgsDesc());
				argsDescReg = CSREG_ARGS_DESC;
			}

			if (insn.id() == ARM64_INS_MOV && insn.ops(1).reg == CSREG_ARGS_DESC) {
				// some function might use ARM64_REG_X1 even ARM64_REG_X0 is free
				argsDescReg = insn.ops(0).reg;
				//INSN_ASSERT(argsDescReg == ARM64_REG_X0 || argsDescReg == ARM64_REG_X1);
				++insn;
			}
			else if (insn.id() == ARM64_INS_LDUR && insn.ops(1).mem.base == CSREG_ARGS_DESC) {
				//std::cout << std::format("  !!! use ArgsDesc directory without moving !!! at {:#x}\n", insn.address());
				argsDescReg = CSREG_ARGS_DESC;
			}
			if (argsDescReg != ARM64_REG_INVALID)
				fnInfo->State()->SetRegister(argsDescReg, fnInfo->Vars()->ValArgsDesc());
		}
		return argsDescReg;
	}();

	if (argsDescReg == ARM64_REG_INVALID && !needSuspendState && !dartFn->IsClosure())
		return nullptr;

	int fixedParamCnt = 0;
	auto paramCntReg = ARM64_REG_INVALID;
	// PrologueBuilder::BuildParameterHandling()
	// it is used only when there is/are optional parameter(s) in a function arguments
	if (optionalParamCntReg == ARM64_REG_INVALID && argsDescReg != ARM64_REG_INVALID) {
		if (insn.id() == ARM64_INS_LDUR && insn.ops(1).mem.base == argsDescReg && insn.ops(1).mem.disp == AOT_ArgumentsDescriptor_count_offset - dart::kHeapObjectTag)
		{
			paramCntReg = ToCapstoneReg(insn.ops(0).reg);
			++insn;

			// the value is Smi object. so decompress pointer instruction is generated automatically even it is not needed.
			handleDecompressPointer(insn, paramCntReg);

			ASSERT(fnInfo->useFramePointer);

			// == positional (fixed) parameters
			// if there is a positional parameter, the number of optional parameters is calculated first.
			// else paramCnt is also the number of optional parameters
			if (insn.id() == ARM64_INS_SUB && insn.ops(1).reg == paramCntReg) {
				fixedParamCnt = (int)insn.ops(2).imm >> dart::kSmiTagShift;
				optionalParamCntReg = insn.ops(0).reg;
				++insn;
			}
		}
	}

	if (optionalParamCntReg != ARM64_REG_INVALID) {
		handleFixedParameters(insn, optionalParamCntReg, fixedParamCnt);
	}

	if (argsDescReg != ARM64_REG_INVALID) {
		// both optional positional parameter and named parameter cannot be used in a function declaration
		// optional positional parameter always start with CMP instruction
		if (insn.id() == ARM64_INS_CMP) {
			// if no fixed parameter, paramCnt is optionalParamCnt
			handleOptionalPositionalParameters(insn, fixedParamCnt > 0 ? optionalParamCntReg : paramCntReg);
		}
		else {
			handleOptionalNamedParameters(insn, paramCntReg);
		}
	}

	// saved parameter in stack might be loaded back to the register
	while (insn.id() == ARM64_INS_LDUR && insn.ops(1).mem.base == CSREG_DART_FP && insn.ops(1).mem.disp < 0) {
		fnInfo->State()->SetRegister(insn.ops(0).reg, fnInfo->State()->GetLocal(insn.ops(1).mem.disp));
		++insn;
	}

	// PrologueBuilder::BuildClosureContextHandling()
	// closure context handling
	if (dartFn->IsClosure() && insn.id() == ARM64_INS_LDUR && insn.ops(1).mem.disp == AOT_Closure_context_offset - dart::kHeapObjectTag) {
		const auto arg1Reg = [&] {
			if (fnInfo->params.numFixedParam > 0) {
				return fnInfo->params[0].valReg;
			}
			else {
				// closure without saving function arguments
				INSN_ASSERT(fnInfo->params.empty());
				return A64::Register{ firstParamReg };
			}
			}();

			if (arg1Reg.IsSet()) {
				INSN_ASSERT(A64::Register{ insn.ops(1).mem.base } == arg1Reg);
				const auto contextReg = ToCapstoneReg(insn.ops(0).reg);
				++insn;

				handleDecompressPointer(insn, contextReg);
				fnInfo->closureContextReg = contextReg;
				// as of now, track only function parameters, so clear the context register as if it is freed
				fnInfo->State()->ClearRegister(fnInfo->closureContextReg);

				const auto storeContextRes = handleStoreLocal(insn, contextReg);
				if (storeContextRes.fpOffset != 0) {
					// save ArgumentsDescriptor to stack
					fnInfo->closureContextLocalOffset = storeContextRes.fpOffset;
				}
			}
	}

	// TypeArgument from Arguments Descriptor might be used
	if (argsDescReg != ARM64_REG_INVALID)
		handleArgumentsDescriptorTypeArguments(insn);

	if (endPrologueAddr != 0 && insn.address() < endPrologueAddr)
		handleInitialization();

	// closure delayed type arguments
	if (dartFn->IsClosure()) {
		const auto save_ins = insn.Current();

		const auto arg1Reg = [&] {
			//int32_t stack_offset = 0;
			arm64_reg a1reg = ARM64_REG_INVALID;
			if (insn.id() == ARM64_INS_LDR && insn.ops(1).mem.base == CSREG_DART_FP && mightBeFirstParamOffset(insn.ops(1).mem.disp)) {
				//stack_offset = insn.ops(1).mem.disp;
				a1reg = insn.ops(0).reg;
				++insn;
			}
			if (insn.id() == ARM64_INS_LDUR && insn.ops(1).mem.disp == AOT_Closure_delayed_type_arguments_offset - dart::kHeapObjectTag) {
				if (a1reg != ARM64_REG_INVALID) {
					// TODO: calculate a number of function parameter
					return A64::Register{ a1reg };
				}
				else if (fnInfo->params.numFixedParam > 0) {
					return fnInfo->params[0].valReg;
				}
				else {
					// closure without saving function arguments
					INSN_ASSERT(fnInfo->params.empty());
					return A64::Register{ firstParamReg };
				}
			}
			return A64::Register{};
		}();

		if (arg1Reg.IsSet() && A64::Register{ insn.ops(1).mem.base } == arg1Reg) {
			fnInfo->State()->SetRegister(arg1Reg, fnInfo->Vars()->ValParam(0));
			auto delayedTypeArgReg = ToCapstoneReg(insn.ops(0).reg);
			++insn;

			handleDecompressPointer(insn, delayedTypeArgReg);

#ifdef NO_METHOD_EXTRACTOR_STUB
			// in this commit, THR::empty_type_arguments is introduced and used here
			INSN_ASSERT(insn.id() == ARM64_INS_LDR);
			INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_THR && insn.ops(1).mem.disp == dart::Thread::empty_type_arguments_offset());
			A64::Register emptyTypeArgReg = insn.ops(0).reg;
			++insn;
#else
			auto ppEmptyTypeArg = getObjectPoolInstruction(insn);
			INSN_ASSERT(ppEmptyTypeArg.IsSet());
			INSN_ASSERT(ppEmptyTypeArg.item.ValueTypeId() == dart::kTypeArgumentsCid);
			A64::Register emptyTypeArgReg = ppEmptyTypeArg.dstReg;
#endif

			INSN_ASSERT(insn.id() == ARM64_INS_CMP);
			INSN_ASSERT(ToCapstoneReg(insn.ops(0).reg) == delayedTypeArgReg);
			INSN_ASSERT(A64::Register{ insn.ops(1).reg } == emptyTypeArgReg);
			fnInfo->State()->ClearRegister(emptyTypeArgReg);
			++insn;

			if (fnInfo->typeArgumentReg.IsSet() && insn.IsBranch(ARM64_CC_NE)) {
				// handle type argument from ArgumentsDescriptor and Closure
				const auto neAddr = insn.ops(0).imm;
				++insn;

				// if (delayedTypeArg == nullTypeArg)
				//   tmp = typeArg_from_ArgsDesc
				INSN_ASSERT(insn.id() == ARM64_INS_MOV);
				INSN_ASSERT(A64::Register{ insn.ops(1).reg } == fnInfo->typeArgumentReg);
				const auto tmpReg = insn.ops(0).reg;
				fnInfo->State()->ClearRegister(tmpReg);
				++insn;

				if (insn.address() == neAddr) {
					delayedTypeArgReg = tmpReg;
					fnInfo->typeArgumentReg = tmpReg;
				}
				else {
					INSN_ASSERT(insn.IsBranch());
					const auto contAddr = insn.ops(0).imm;
					++insn;

					INSN_ASSERT(insn.address() == neAddr);

					INSN_ASSERT(insn.id() == ARM64_INS_MOV);
					INSN_ASSERT(insn.ops(1).reg == delayedTypeArgReg);
					INSN_ASSERT(insn.ops(0).reg == tmpReg);
					delayedTypeArgReg = insn.ops(0).reg;
					++insn;

					// no check because delayedTypeArgReg is a result of selection between type argument from ArgumentsDescript and Closure
					fnInfo->typeArgumentReg = delayedTypeArgReg;

					INSN_ASSERT(insn.address() == contAddr);
				}
			}
			else {
				INSN_ASSERT(insn.IsBranch(ARM64_CC_EQ));
				const auto contAddr = insn.ops(0).imm;
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_MOV);
				INSN_ASSERT(insn.ops(1).reg == delayedTypeArgReg);
				delayedTypeArgReg = insn.ops(0).reg;
				++insn;

				INSN_ASSERT(!fnInfo->typeArgumentReg.IsSet() || fnInfo->typeArgumentReg == A64::Register{ delayedTypeArgReg });
				fnInfo->typeArgumentReg = delayedTypeArgReg;

				INSN_ASSERT(insn.address() == contAddr);
			}

			// as of now, track only function parameters, so clear the type argument register as if it is freed
			fnInfo->State()->ClearRegister(fnInfo->typeArgumentReg);
		}
		else {
			insn.SetCurrent(save_ins);
		}
	}

	if (insn.address() < endPrologueAddr) {
		const auto addRegisterFunctionParameter = [&](A64::Register srcReg) {
			// only R1, R2 and R3 for function arguments
			INSN_ASSERT(srcReg == A64::Register::R1 || srcReg == A64::Register::R2 || srcReg == A64::Register::R3);
			const auto argIdx = (int)srcReg - 1;
			auto val = fnInfo->Vars()->ValParam(argIdx);
			// set srcReg as function argument (previous should be nullptr)
			fnInfo->State()->SetRegister(srcReg, val);
			// assume register argument is used in order
			INSN_ASSERT(fnInfo->params.NumParam() == argIdx);
			auto paramInfo = FnParamInfo{ srcReg };
			if (!dartFn->IsStatic() && argIdx == 0) {
				// class method. first parameter is "this"
				paramInfo.name = "this";
				paramInfo.type = dartFn->Class().DeclarationType(); // TODO: class with type arguments (generic class)
			}
			fnInfo->params.add(std::move(paramInfo));
			fnInfo->params.numFixedParam = argIdx + 1;
			return val;
		};

		// moving registers
		while (insn.id() == ARM64_INS_MOV && insn.ops(1).type == ARM64_OP_REG && insn.ops(1).reg != CSREG_DART_NULL) {
			const A64::Register srcReg = insn.ops(1).reg;
			const A64::Register dstReg = insn.ops(0).reg;
			const auto val = fnInfo->State()->MoveRegister(dstReg, srcReg);
			if (val == nullptr) {
				auto val = addRegisterFunctionParameter(srcReg);
				fnInfo->State()->SetRegister(dstReg, val);
			}
			++insn;
		}
		// before CheckStackOverflow, there might be loading paramters into registers and storing some register to local stack
		// the parameter might be loaded to a register
		while (insn.id() == ARM64_INS_LDR && insn.ops(1).mem.base == CSREG_DART_FP && insn.ops(1).mem.disp > 0) {
			const auto dst_reg = insn.ops(0).reg;
			const auto offset = insn.ops(1).mem.disp;
			++insn;
			if (insn.id() == ARM64_INS_LDUR && insn.ops(1).mem.base == dst_reg && insn.ops(1).mem.disp > 0) {
				// load parameter to load a object field
				// TODO: it might be load this object field to a register
				--insn;
				break;
			}
			const auto paramIdx = (offset - sizeof(void*) * 2) / sizeof(void*);
			fnInfo->State()->SetRegister(dst_reg, fnInfo->Vars()->ValParam(paramIdx));
		}
		handleInitialization();
		// the value might be saved to stack as if it is a local variable but with fixed negative offset from FP
		while (true) {
			const auto storeRes = handleStoreLocal(insn);
			if (storeRes.fpOffset == 0)
				break;

			const auto srcReg = A64::Register{ storeRes.srcReg };
			if (fnInfo->typeArgumentReg == srcReg) {
				fnInfo->typeArgumentLocalOffset = storeRes.fpOffset;
			}
			else if (fnInfo->closureContextReg == srcReg) {
				fnInfo->closureContextLocalOffset = storeRes.fpOffset;
			}
			else {
				auto val = fnInfo->State()->GetValue(srcReg);
				if (val == nullptr) {
					//std::cout << std::format("Cannot find define of srcReg\n");
					//auto ins = insn.Current() - 1;
					//std::cout << std::format("  {:#x}: {} {}\n", ins->address, &ins->mnemonic[0], &ins->op_str[0]);
					INSN_ASSERT(dartFn->IsClosure() || needSuspendState);
					val = addRegisterFunctionParameter(srcReg);
				}
				fnInfo->State()->SetLocal(storeRes.fpOffset, val);
			}
		}
	}

	// set all parameter register value and local variable
	if (!fnInfo->params.empty()) {
		// clear all valRegs in param first
		for (auto& param : fnInfo->params.params) {
			param.valReg = A64::Register{};
			param.localOffset = 0;
		}
		const auto& local_vars = fnInfo->State()->local_vars;
		for (auto i = 0; i < local_vars.size(); i++) {
			const auto local = local_vars[i];
			if (local && local->RawTypeId() == VarValue::Parameter) {
				fnInfo->params[local->AsParam()->idx].localOffset = AnalyzingState::indexToLocalOffset(i);
			}
		}
		auto& regs = fnInfo->State()->regs;
		for (auto i = 0; i < A64::Register::kNumberOfRegisters; i++) {
			if (regs[i] && regs[i]->RawTypeId() == VarValue::Parameter) {
				fnInfo->params[regs[i]->AsParam()->idx].valReg = A64::Register::Value{ i };
			}
		}
	}

	if (insn.Current() == marker.Insn())
		return nullptr;

	return std::make_unique<SetupParametersInstr>(insn.Wrap(marker.Take()), &fnInfo->params);
}

std::unique_ptr<LoadValueInstr> FunctionAnalyzer::processLoadValueInstr(AsmIterator& insn)
{
	const auto ins0_addr = insn.address();
	auto objPoolInstr = getObjectPoolInstruction(insn);
	if (objPoolInstr.IsSet()) {
		// TODO: load object might be start of Dart instruction, check next instruction
		return std::make_unique<LoadValueInstr>(insn.Wrap(ins0_addr), objPoolInstr.dstReg, std::move(objPoolInstr.item));
	}

	int64_t imm = 0;
	A64::Register dstReg;
	// libcapstone5 use MOV instead of MOVZ
	if (insn.IsMovz()) {
		imm = insn.ops(1).imm;
		const auto tmpReg = insn.ops(0).reg;
		++insn;

		if (insn.id() == ARM64_INS_MOVK && insn.ops(0).reg == tmpReg && insn.ops(1).shift.value == 16) {
			imm |= insn.ops(1).imm << 16;
			++insn;
		}
		dstReg = tmpReg;
	}
	else if (insn.id() == ARM64_INS_MOV && insn.ops(1).reg == CSREG_DART_NULL) {
		auto item = VarItem{ VarStorage::Immediate, new VarNull() };
		const auto dstReg = A64::Register{ insn.ops(0).reg };
		++insn;
		return std::make_unique<LoadValueInstr>(insn.Wrap(ins0_addr), dstReg, std::move(item));
	}
	else if (insn.id() == ARM64_INS_ORR && insn.ops(1).reg == ARM64_REG_XZR && insn.ops(2).type == ARM64_OP_IMM) {
		imm = insn.ops(2).imm;
		dstReg = insn.ops(0).reg;
		++insn;
	}
	else if (insn.id() == ARM64_INS_MOVN) {
		imm = insn.ops(1).imm << insn.ops(1).shift.value;
		imm = ~imm;
		dstReg = insn.ops(0).reg;
		++insn;
	}
	else if (insn.id() == ARM64_INS_EOR && insn.ops(0).reg == insn.ops(1).reg && insn.ops(0).reg == insn.ops(2).reg) {
		dstReg = insn.ops(0).reg;
		++insn;
		if (dstReg.IsDecimal()) {
			auto item = VarItem{ VarStorage::Immediate, new VarDouble{0, VarValue::NativeDouble} };
			return std::make_unique<LoadValueInstr>(insn.Wrap(ins0_addr), dstReg, std::move(item));
		}
	}
	else if (insn.id() == ARM64_INS_FMOV) {
		auto item = VarItem{ VarStorage::Immediate, new VarDouble{insn.ops(1).fp, VarValue::NativeDouble} };
		const auto dstReg = A64::Register{ insn.ops(0).reg };
		++insn;
		return std::make_unique<LoadValueInstr>(insn.Wrap(ins0_addr), dstReg, std::move(item));
	}

	if (dstReg.IsSet()) {
		auto item = VarItem{ VarStorage::Immediate, new VarInteger{imm, VarValue::NativeInt} };
		return std::make_unique<LoadValueInstr>(insn.Wrap(ins0_addr), dstReg, std::move(item));
	}

	return nullptr;
}

std::unique_ptr<ClosureCallInstr> FunctionAnalyzer::processClosureCallInstr(AsmIterator& insn)
{
	// ClosureCallInstr::EmitNativeCode
	if (insn.id() == ARM64_INS_LDUR && insn.ops(0).reg == ARM64_REG_X2 && insn.ops(1).mem.base == ARM64_REG_X0 && insn.ops(1).mem.disp == AOT_Closure_entry_point_offset - dart::kHeapObjectTag) {
		// previous IL must be Load from PP and it is list for ArgumentDescriptor and dstReg is R4
		auto il = fnInfo->LastIL();
		if (il->Kind() == ILInstr::LoadValue) {
			auto loadIL = reinterpret_cast<LoadValueInstr*>(il);
			if (loadIL->dstReg == A64::Register{ dart::ARGS_DESC_REG } && loadIL->val.ValueTypeId() == dart::kArrayCid) {
				const auto& arr = dart::Array::Handle(loadIL->val.Get<VarArray>()->ptr);
				const auto arrLen = arr.Length();
				const auto namedParamCmt = (arrLen - 5) / 2;
				auto arrPtr = dart::Array::DataOf(arr.ptr());

				INSN_ASSERT(!arrPtr->IsHeapObject());
				const auto typeArgLen = dart::RawSmiValue(dart::Smi::RawCast(arrPtr->DecompressSmi()));
				arrPtr++;
				INSN_ASSERT(!arrPtr->IsHeapObject());
				const auto argCnt = dart::RawSmiValue(dart::Smi::RawCast(arrPtr->DecompressSmi()));
				INSN_ASSERT(argCnt > 0);
				arrPtr++;
				INSN_ASSERT(!arrPtr->IsHeapObject());
				const auto argSize = dart::RawSmiValue(dart::Smi::RawCast(arrPtr->DecompressSmi()));
				INSN_ASSERT(argCnt == argSize);
				arrPtr++;
				INSN_ASSERT(!arrPtr->IsHeapObject());
				const auto positionalArgCnt = dart::RawSmiValue(dart::Smi::RawCast(arrPtr->DecompressSmi()));
				INSN_ASSERT(argCnt == positionalArgCnt + namedParamCmt);
				// TODO: extract parameter name
				//arrPtr++;
				//INSN_ASSERT(arrPtr->IsHeapObject());
				//INSN_ASSERT(arrPtr->Decompress(app.heap_base()).IsRawNull());

				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_BLR);
				INSN_ASSERT(insn.ops(0).reg == ARM64_REG_X2);
				++insn;

				const auto ins0_addr = il->Start();
				fnInfo->RemoveLastIL();

				// TODO: still don't know what information is needed for full analysis
				return std::make_unique<ClosureCallInstr>(insn.Wrap(ins0_addr), (int32_t)argCnt, (int32_t)typeArgLen);
			}
		}
	}

	return nullptr;
}

std::unique_ptr<MoveRegInstr> FunctionAnalyzer::processMoveRegInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_MOV && insn.ops(1).type == ARM64_OP_REG) {
		const auto ins0_addr = insn.address();
		const A64::Register dstReg = insn.ops(0).reg;
		const A64::Register srcReg = insn.ops(1).reg;
		++insn;
		return std::make_unique<MoveRegInstr>(insn.Wrap(ins0_addr), dstReg, srcReg);
	}
	return nullptr;
}

std::unique_ptr<DecompressPointerInstr> FunctionAnalyzer::processDecompressPointerInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_ADD && insn.ops(2).reg == CSREG_DART_HEAP && insn.ops(2).shift.value == 32) {
		INSN_ASSERT(insn.ops(0).reg == insn.ops(1).reg);
		const auto reg = A64::Register{ insn.ops(0).reg };
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<DecompressPointerInstr>(insn.Wrap(ins0_addr), reg);
	}
	return nullptr;
}

std::unique_ptr<SaveRegisterInstr> FunctionAnalyzer::processSaveRegisterInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_STR && insn.ops(1).mem.base == CSREG_DART_SP && insn.writeback()) {
		INSN_ASSERT(insn.ops(1).mem.disp == -GetCsRegSize(insn.ops(0).reg));
		const auto reg = A64::Register{ insn.ops(0).reg };
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<SaveRegisterInstr>(insn.Wrap(ins0_addr), reg);
	}
	return nullptr;
}

std::unique_ptr<RestoreRegisterInstr> FunctionAnalyzer::processLoadSavedRegisterInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_LDR && insn.ops(1).mem.base == CSREG_DART_SP && insn.writeback()) {
		INSN_ASSERT(insn.ops(2).imm == GetCsRegSize(insn.ops(0).reg));
		const auto reg = A64::Register{ insn.ops(0).reg };
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<RestoreRegisterInstr>(insn.Wrap(ins0_addr), reg);
	}
	return nullptr;
}

std::unique_ptr<InitAsyncInstr> FunctionAnalyzer::processInitAsyncInstr(AsmIterator& insn)
{
#ifdef HAS_INIT_ASYNC
	// InitAsync cannot be tail jump
	if (insn.id() == ARM64_INS_BL && insn.ops(0).type == ARM64_OP_IMM) {
		const auto fn = app.GetFunction(insn.ops(0).imm);
		if (fn && fn->IsStub()) {
			const auto stub = fn->AsStub();
			auto il = fnInfo->LastIL();
			if (stub->kind == DartStub::InitAsyncStub && il->Kind() == ILInstr::LoadValue) {
				auto ilLoad = reinterpret_cast<LoadValueInstr*>(il);
				INSN_ASSERT(ilLoad->dstReg == A64::Register::R0);
				auto& item = ilLoad->GetValue();
				DartType* returnType;
				if (item.ValueTypeId() == dart::kNullCid) {
					// Future<Null>
					returnType = app.TypeDb()->FindOrAdd(app.DartFutureCid(), &DartTypeArguments::Null);
				}
				else {
					INSN_ASSERT(item.ValueTypeId() == dart::kTypeArgumentsCid);
					auto typeArg = &(item.Get<VarTypeArgument>()->typeArgs);
					returnType = app.TypeDb()->FindOrAdd(app.DartFutureCid(), typeArg);
				}
				fnInfo->returnType = returnType;
				const auto start = il->Start();
				fnInfo->RemoveLastIL();
				++insn;
				return std::make_unique<InitAsyncInstr>(insn.Wrap(start), returnType);
			}
		}
	}
#endif

	return nullptr;
}

std::unique_ptr<CallInstr> FunctionAnalyzer::processCallInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_BL) {
		if (insn.ops(0).type == ARM64_OP_IMM) {
			const auto target = (uint64_t)insn.ops(0).imm;
			const auto ins0_addr = insn.address();
			setAsmTextDataCall(ins0_addr, target);
			++insn;
			return std::make_unique<CallInstr>(insn.Wrap(ins0_addr), app.GetFunction(target), target);
		}
		// should indirect call handle here
	}
	else if (insn.id() == ARM64_INS_B && insn.ops(0).type == ARM64_OP_IMM) {
		// this might tail branch (function call at the end of function)
		const auto target = (uint64_t)insn.ops(0).imm;
		if (target < dartFn->Address() || target >= dartFn->AddressEnd()) {
			const auto ins0_addr = insn.address();
			setAsmTextDataCall(ins0_addr, target);
			++insn;
			return std::make_unique<CallInstr>(insn.Wrap(ins0_addr), app.GetFunction(target), target);
		}
	}

	return nullptr;
}

std::unique_ptr<GdtCallInstr> FunctionAnalyzer::processGdtCallInstr(AsmIterator& insn)
{
	// https://mrale.ph/dartvm/#global-dispatch-table-gdt
	// FlowGraphCompiler::EmitDispatchTableCall()
	//     const intptr_t offset = selector_offset - DispatchTable::kOriginElement;
	//     __ AddImmediate(LR, cid_reg, offset);
	//   so it might be addition depended on selector offset (but most of them is subtraction)
	// 0x26de04: sub  x30, x0, #0xfda
	// 0x26de08: ldr  x30, [x21, x30, lsl #3]
	// 0x26de0c: blr  x30
	//   or
	// 0x220284: movz  x17, #0x1545  ; an extra instruction when offset is larger than 0x1000
	// 0x220288: add  x30, x0, x17
	// 0x22028c: ldr  x30, [x21, x30, lsl #3]
	// 0x220290: blr  x30
	if (insn.ops(0).reg == CSREG_DART_LR && (insn.id() == ARM64_INS_ADD || insn.id() == ARM64_INS_SUB) &&
		insn.ops(1).reg == ToCapstoneReg(dart::DispatchTableNullErrorABI::kClassIdReg))
	{
		// this is GDT call
		auto insn0_addr = insn.address();
		int64_t offset = 0;

		if (insn.ops(2).type == ARM64_OP_IMM) {
			//cidReg = insn.ops(1).reg;
			offset = insn.ops(2).imm;
			if (insn.ops(2).shift.type != ARM64_SFT_INVALID) {
				ASSERT(insn.ops(2).shift.type == ARM64_SFT_LSL);
				offset <<= insn.ops(2).shift.value;
			}
			if (insn.id() == ARM64_INS_SUB)
				offset = -offset;
			++insn;
		}
		else {
			// from reg
			INSN_ASSERT(insn.id() == ARM64_INS_ADD);
			INSN_ASSERT(insn.ops(2).type == ARM64_OP_REG && insn.ops(2).reg == CSREG_DART_TMP2);

			auto il_loadImm = reinterpret_cast<LoadValueInstr*>(fnInfo->LastIL());
			INSN_ASSERT(il_loadImm->val.Storage().IsImmediate());
			INSN_ASSERT(il_loadImm->dstReg == A64::TMP2_REG);
			offset = il_loadImm->val.Get<VarInteger>()->Value();
			insn0_addr = il_loadImm->Start();
			fnInfo->RemoveLastIL();
			++insn;
		}
		//const auto selector_offset = dart::DispatchTable::kOriginElement + offset;

		INSN_ASSERT(insn.id() == ARM64_INS_LDR);
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
		ASSERT(insn.ops(1).mem.base == CSREG_DART_DISPATCH_TABLE && insn.ops(1).mem.index == CSREG_DART_LR && insn.ops(1).shift.value == 3);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_BLR);
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
		++insn;

		return std::make_unique<GdtCallInstr>(insn.Wrap(insn0_addr), offset);
	}

	return nullptr;
}

std::unique_ptr<ReturnInstr> FunctionAnalyzer::processReturnInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_RET) {
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<ReturnInstr>(insn.Wrap(ins0_addr));
	}
	return nullptr;
}

std::unique_ptr<TestTypeInstr> FunctionAnalyzer::processInstanceofNoTypeArgumentInstr(AsmIterator& insn)
{
	// cannot find where the compiler generate setting kInstanceReg, kInstantiatorTypeArgumentsReg and kFunctionTypeArgumentsReg code
	// FlowGraphCompiler::GenerateInstanceOf()
	// - FlowGraphCompiler::GenerateInlineInstanceof()
	//   - FlowGraphCompiler::GenerateInstantiatedTypeNoArgumentsTest()
	InsnMarker marker(insn);
	const auto srcReg = [&] {
		if (insn.id() == ARM64_INS_MOV && insn.ops(0).reg == ToCapstoneReg(dart::TypeTestABI::kInstanceReg)) {
			const auto srcReg = insn.ops(1).reg;
			++insn;
			if (insn.id() == ARM64_INS_MOV && insn.ops(0).reg == ToCapstoneReg(dart::TypeTestABI::kInstantiatorTypeArgumentsReg) && insn.ops(1).reg == CSREG_DART_NULL) {
				++insn;
				if (insn.id() == ARM64_INS_MOV && insn.ops(0).reg == ToCapstoneReg(dart::TypeTestABI::kFunctionTypeArgumentsReg) && insn.ops(1).reg == CSREG_DART_NULL) {
					++insn;
					return srcReg;
				}
			}
		}
		return ARM64_REG_INVALID;
	}();

	if (srcReg != ARM64_REG_INVALID) {
		// ; branchIfSmi()
		// 0x295a2c: tbz  w0, #0, #0x295a54
		// ; LoadClassId()
		// 0x295a30: ldur  x4, [x0, #-1]
		// 0x295a34: ubfx  x4, x4, #0xc, #0x14
		// 0x295a38: sub  x4, x4, #0x3c
		// 0x295a3c: cmp  x4, #1
		// 0x295a40: b.ls  #0x295a54
		// 0x295a44: ldr  x8, [x27, #0x718]  (PP+0x718 - Type: int)
		// 0x295a48: add  x3, x27, #9, lsl #12  (PP+0x9db0 - Null)
		// 0x295a4c: ldr  x3, [x3, #0xdb0]
		// 0x295a50: bl  #0x3022fc  # IsType_int_Stub
		const auto ilBranch = processBranchIfSmiInstr(insn);
		// TODO: there might be check for NULL here
		if (!ilBranch)
			return nullptr;
		INSN_ASSERT(ilBranch->objReg == A64::Register{ dart::TypeTestABI::kInstanceReg });
		const auto done_addr = ilBranch->branchAddr;

		// int and num type are extracted for quick check. other object just use stub for checking.
		intptr_t typeCheckCid = 0;
		const auto ilLoadCid = processLoadClassIdInstr(insn);
		if (ilLoadCid) {
			INSN_ASSERT(ilLoadCid->objReg == A64::Register{ dart::TypeTestABI::kInstanceReg });
			INSN_ASSERT(ilLoadCid->cidReg == A64::Register{ dart::TypeTestABI::kScratchReg });

			INSN_ASSERT(insn.id() == ARM64_INS_SUB);
			INSN_ASSERT(insn.ops(0).reg == ToCapstoneReg(dart::TypeTestABI::kScratchReg));
			INSN_ASSERT(insn.ops(1).reg == ToCapstoneReg(dart::TypeTestABI::kScratchReg));
			INSN_ASSERT(insn.ops(2).imm == dart::kSmiCid); // kMintCid is always be kSmiCid+1
			++insn;

			INSN_ASSERT(insn.id() == ARM64_INS_CMP);
			INSN_ASSERT(insn.ops(0).reg == ToCapstoneReg(dart::TypeTestABI::kScratchReg));
			INSN_ASSERT(insn.ops(1).imm == 1 || insn.ops(1).imm == 2); // must be kSmiCid or kMintCid or kDoubleCid
			typeCheckCid = insn.ops(1).imm == 1 ? app.DartIntCid() : dart::kNumberCid;
			++insn;

			INSN_ASSERT(insn.IsBranch(ARM64_CC_LS));
			INSN_ASSERT(insn.ops(0).imm == done_addr);
			++insn;
		}

		// kDstTypeReg from PP
		const auto objPoolInstr = getObjectPoolInstruction(insn);
		INSN_ASSERT(objPoolInstr.dstReg == A64::Register{ dart::TypeTestABI::kDstTypeReg });
		INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kTypeCid);
		const auto vtype = objPoolInstr.item.Get<VarType>();
		INSN_ASSERT(typeCheckCid == 0 || typeCheckCid == vtype->type.Class().Id());

		auto test_ep_reg = ARM64_REG_INVALID;
		if (insn.id() == ARM64_INS_LDUR) {
			INSN_ASSERT(insn.ops(1).mem.base == ToCapstoneReg(dart::TypeTestABI::kDstTypeReg));
			INSN_ASSERT(insn.ops(1).mem.disp == AOT_AbstractType_type_test_stub_entry_point_offset - dart::kHeapObjectTag);
			test_ep_reg = insn.ops(0).reg;
			++insn;
		}

		// kSubtypeTestCacheReg from PP
		const auto objPoolInstr2 = getObjectPoolInstruction(insn);
		INSN_ASSERT(objPoolInstr2.dstReg == A64::Register{ dart::TypeTestABI::kSubtypeTestCacheReg });
		INSN_ASSERT(objPoolInstr2.item.Value()->RawTypeId() == dart::kNullCid);

		if (test_ep_reg == ARM64_REG_INVALID) {
			INSN_ASSERT(insn.id() == ARM64_INS_BL);
			auto dartFn = app.GetFunction(insn.ops(0).imm);
			auto dartStub = dartFn->AsStub();
			auto typeName = dartStub->Name();
			if (typeCheckCid == app.DartIntCid()) {
				INSN_ASSERT(dartStub->kind == DartStub::TypeCheckStub);
				INSN_ASSERT(typeName == "int" || typeName == "int?");
			}
			else { // typeCheckCid == dart::kNumberCid || typeCheckCid == 0 (object)
				INSN_ASSERT(typeName == vtype->ToString() || dartStub->kind == DartStub::DefaultTypeTestStub || dartStub->kind == DartStub::DefaultNullableTypeTestStub);
			}
			++insn;
		}
		else {
			INSN_ASSERT(insn.id() == ARM64_INS_BLR);
			INSN_ASSERT(insn.ops(0).reg == test_ep_reg);
			++insn;
		}

		INSN_ASSERT(insn.address() == done_addr);

		return std::make_unique<TestTypeInstr>(insn.Wrap(marker.Take()), A64::Register{srcReg}, objPoolInstr.item.Get<VarType>()->ToString());
	}

	return nullptr;
}

std::unique_ptr<BranchIfSmiInstr> FunctionAnalyzer::processBranchIfSmiInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_TBZ && insn.ops(1).imm == dart::kSmiTag && dart::kCompressedWordSize == GetCsRegSize(insn.ops(0).reg)) {
		const auto objReg = A64::Register{ insn.ops(0).reg };
		const auto branchAddr = insn.ops(2).imm;
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<BranchIfSmiInstr>(insn.Wrap(ins0_addr), objReg, branchAddr);
	}
	return nullptr;
}

std::unique_ptr<LoadClassIdInstr> FunctionAnalyzer::processLoadClassIdInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_LDUR && insn.ops(1).mem.disp == -1 && dart::UntaggedObject::kClassIdTagPos == 12) {
		// 0x21cb10: ldur  x1, [x0, #-1]  ; load object tag
		const auto objReg = A64::Register{ insn.ops(1).mem.base };
		const auto cidReg = insn.ops(0).reg;
		const auto ins0_addr = insn.address();
		++insn;

		// Assembler::ExtractClassIdFromTags()
		// 0x21cb14: ubfx  x1, x1, #0xc, #0x14  ; extract object class id
		INSN_ASSERT(insn.id() == ARM64_INS_UBFX);
		INSN_ASSERT(insn.ops(0).reg == cidReg);
		INSN_ASSERT(insn.ops(1).reg == cidReg);
		INSN_ASSERT(insn.ops(2).imm == dart::UntaggedObject::kClassIdTagPos);
		INSN_ASSERT(insn.ops(3).imm == dart::UntaggedObject::kClassIdTagSize);
		++insn;

		return std::make_unique<LoadClassIdInstr>(insn.Wrap(ins0_addr), objReg, A64::Register{cidReg});
	}
	else if (insn.id() == ARM64_INS_LDURH && insn.ops(1).mem.disp == 1 && dart::UntaggedObject::kClassIdTagPos == 16) {
		// https://github.com/dart-lang/sdk/commit/9182d5e5359988703a2b8a88c238f47a5295e18c
		// 0x5f3900: ldurh  w3, [x0, #1]
		const auto objReg = A64::Register{ insn.ops(1).mem.base };
		const auto cidReg = insn.ops(0).reg;
		const auto ins0_addr = insn.address();
		++insn;

		return std::make_unique<LoadClassIdInstr>(insn.Wrap(ins0_addr), objReg, A64::Register{cidReg});
	}
	return nullptr;
}

std::unique_ptr<LoadTaggedClassIdMayBeSmiInstr> FunctionAnalyzer::processLoadTaggedClassIdMayBeSmiInstr(AsmIterator& insn)
{
	//   cidReg = SmiTaggedClassId
	//   branchIfSmi(objReg, branchAddr)
	//   cidReg = LoadClassIdInstr(objReg)
	//   lsl  cidReg, cidReg, #1
	// branchAddr:
	auto& il = fnInfo->il_insns.back();
	if (insn.id() == ARM64_INS_LSL && insn.ops(0).reg == insn.ops(1).reg && insn.ops(2).imm == dart::kSmiTagSize && il->Kind() == ILInstr::LoadClassId && fnInfo->il_insns.size() >= 3) {
		auto il_loadClassId = reinterpret_cast<LoadClassIdInstr*>(il.get());
		if (il_loadClassId->cidReg == A64::Register{ insn.ops(0).reg }) {
			auto& il2 = fnInfo->il_insns[fnInfo->il_insns.size() - 2];
			if (il2->Kind() == ILInstr::BranchIfSmi) {
				auto il_branchIfSmi = reinterpret_cast<BranchIfSmiInstr*>(il2.get());
				INSN_ASSERT(il_branchIfSmi->objReg == il_loadClassId->objReg);
				auto& il3 = fnInfo->il_insns[fnInfo->il_insns.size() - 3];
				INSN_ASSERT(il3->Kind() == ILInstr::LoadValue);
				auto il_loadImm = reinterpret_cast<LoadValueInstr*>(il3.get());
				INSN_ASSERT(il_loadImm->dstReg == il_loadClassId->cidReg);
				INSN_ASSERT(il_loadImm->val.Storage().IsImmediate());
				INSN_ASSERT(il_loadImm->val.ValueTypeId() == dart::kIntegerCid);
				INSN_ASSERT(il_loadImm->val.Get<VarInteger>()->Value() == dart::Smi::RawValue(dart::kSmiCid));

				// everything is OK, release all IL to cast to specific IL. Then, put them into a new IL
				il.release();
				il2.release();
				il3.release();
				++insn;
				auto il_new = std::make_unique<LoadTaggedClassIdMayBeSmiInstr>(insn.Wrap(il_loadImm->Start()),
					std::unique_ptr<LoadValueInstr>(il_loadImm), std::unique_ptr<BranchIfSmiInstr>(il_branchIfSmi),
					std::unique_ptr<LoadClassIdInstr>(il_loadClassId));
				fnInfo->il_insns.resize(fnInfo->il_insns.size() - 3);
				return std::move(il_new);
			}
		}
	}
	return nullptr;
}

std::unique_ptr<BoxInt64Instr> FunctionAnalyzer::processBoxInt64Instr(AsmIterator& insn)
{
	// From BoxInt64Instr::EmitNativeCode()
	// Note: without compressed pointer, start instruction is "adds"
	// 0x21c5e8: sbfiz  x0, x2, #1, #0x1f
	// 0x21c5ec: cmp  x2, x0, asr #1
	// 0x21c5f0: b.eq  #0x21c5fc
	// 0x21c5f4: bl  #0x2e5500  # AllocateMintSharedWithoutFPURegsStub
	// 0x21c5f8: stur  x2, [x0, #7]
	//   x0 is result object (can be Smi or Mint). x2 is source integer.
	if (insn.id() == ARM64_INS_SBFIZ && insn.ops(2).imm == dart::kSmiTagSize && insn.ops(3).imm == 31) {
		const auto out_reg = insn.ops(0).reg;
		const auto in_reg = insn.ops(1).reg;
		InsnMarker marker(insn);
		++insn;

		if (insn.id() == ARM64_INS_CMP && insn.ops(0).reg == in_reg && insn.ops(1).reg == out_reg && insn.ops(1).shift.type == ARM64_SFT_ASR && insn.ops(1).shift.value == dart::kSmiTagSize) {
			// branch if integer value is fit in 31 bits
			++insn;

			INSN_ASSERT(insn.id() == ARM64_INS_B && insn.cc() == ARM64_CC_EQ);
			const auto contAddr = insn.ops(0).imm;
			++insn;

			const auto assertAllocateMintStub = [&](DartFnBase* stub) {
				INSN_ASSERT(stub->IsStub());
				const auto stubKind = reinterpret_cast<DartStub*>(stub)->kind;
				INSN_ASSERT(stubKind == DartStub::AllocateMintSharedWithoutFPURegsStub || stubKind == DartStub::AllocateMintSharedWithFPURegsStub);
			};

			if (insn.id() == ARM64_INS_BL) {
				assertAllocateMintStub(app.GetFunction(insn.ops(0).imm));
				++insn;
			}
			else {
				const auto objPoolInstr = getObjectPoolInstruction(insn);
				INSN_ASSERT(objPoolInstr.IsSet());
				INSN_ASSERT(objPoolInstr.dstReg == A64::Register{ dart::CODE_REG });
				INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kFunctionCid);
				assertAllocateMintStub(&objPoolInstr.item.Get<VarFunctionCode>()->fn);

				INSN_ASSERT(insn.id() == ARM64_INS_LDUR);
				INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
				INSN_ASSERT(insn.ops(1).mem.base == ToCapstoneReg(dart::CODE_REG) && insn.ops(1).mem.disp == AOT_Code_entry_point_offset[(int)dart::CodeEntryKind::kNormal] - dart::kHeapObjectTag);
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_BLR);
				INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
				++insn;
			}

			INSN_ASSERT(insn.id() == ARM64_INS_STUR);
			INSN_ASSERT(insn.ops(0).reg == in_reg);
			INSN_ASSERT(insn.ops(1).mem.base == out_reg && insn.ops(1).mem.disp == dart::Mint::value_offset() - dart::kHeapObjectTag);
			++insn;

			INSN_ASSERT(insn.address() == contAddr);

			const auto objReg = A64::Register{ out_reg };
			const auto srcReg = A64::Register{ in_reg };
			return std::make_unique<BoxInt64Instr>(insn.Wrap(marker.Take()), objReg, srcReg);
		}
	}
	return nullptr;
}

std::unique_ptr<LoadInt32Instr> FunctionAnalyzer::processLoadInt32FromBoxOrSmiInstrFromSrcReg(AsmIterator& insn, A64::Register expectedSrcReg)
{
	// From UnboxInstr::EmitLoadInt32FromBoxOrSmi(), includes branch if Smi
	//   output is raw integer, input is Dart object
	if (insn.id() == ARM64_INS_SBFX && insn.ops(2).imm == dart::kSmiTagSize && insn.ops(3).imm == 31) {
		const auto in_reg = insn.ops(1).reg;
		const auto srcReg = A64::Register{ in_reg };
		if (!expectedSrcReg.IsSet() || expectedSrcReg == srcReg) {
			const auto out_reg = insn.ops(0).reg;
			const auto dstReg = A64::Register{ out_reg };
			const auto ins0_addr = insn.address();
			++insn;
			// TODO: make srcReg type in current function register to be Smi

			if (insn.id() == ARM64_INS_TBZ && A64::Register{ insn.ops(0).reg } == srcReg && insn.ops(1).imm == dart::kSmiTag) {
				// if not smi, get raw integer from Mint object
				// these instructions might not be here because compiler know the value is always be Smi
				const auto cont_addr = insn.ops(2).imm;
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_LDUR);
				INSN_ASSERT(insn.ops(0).reg == out_reg);
				INSN_ASSERT(insn.ops(1).mem.base == in_reg && insn.ops(1).mem.disp == dart::Mint::value_offset() - dart::kHeapObjectTag);
				++insn;

				// srcReg item type MUST be Mint
				INSN_ASSERT(insn.address() == cont_addr);
			}
			else {
				// srcReg object is always Smi
			}

			return std::make_unique<LoadInt32Instr>(insn.Wrap(ins0_addr), dstReg, srcReg);
		}
	}
	return nullptr;
}

std::unique_ptr<LoadInt32Instr> FunctionAnalyzer::processLoadInt32FromBoxOrSmiInstr(AsmIterator& insn)
{
	return processLoadInt32FromBoxOrSmiInstrFromSrcReg(insn);
}

std::unique_ptr<ILInstr> FunctionAnalyzer::processLoadFieldTableInstr(AsmIterator& insn)
{
	if (insn.id() == ARM64_INS_LDR && insn.ops(1).mem.base == CSREG_DART_THR && insn.ops(1).mem.disp == AOT_Thread_field_table_values_offset) {
		// LoadStaticFieldInstr::EmitNativeCode()
		// 0x21cb80: ldr  x0, [x26, #0x68]  (Thread::field_table_values)
		//    ; might have an extra add if field_offset is larger than 0x8000 => add x17, x0, #8, lsl #12
		// 0x21cb84: ldr  x0, [x0, #0x1730]  ; Smi(field_offset) = 0x1730
		// 0x21cb88: ldr  x16, [x27, #0x28]  (PP+0x28 - XXX: sentinel (30))
		// 0x21cb8c: cmp  w0, w16
		// 0x21cb90: b.ne  #0x21cba0
		// 0x21cb94: add  x2, x27, #9, lsl #12  (PP+0x9a18 - Field <_MyHomePageState@782102494.factor>: static late (offset: 0xb98))
		// 0x21cb98: ldr  x2, [x2, #0xa18]
		// 0x21cb9c: bl  #0x2e36cc  # InitLateStaticFieldStub
		//   --
		// 0x4503c0: ldr  x1, [x26, #0x68](Thread::field_table_values)
		// 0x4503c4: str  x0, [x1, #0x1250]  ; Set static field
		const auto result_reg = insn.ops(0).reg;
		const auto dstReg = A64::Register{ result_reg };
		auto tmp_reg = insn.ops(0).reg;
		auto load_offset = 0;
		InsnMarker marker(insn);
		++insn;

		if (insn.id() == ARM64_INS_ADD && insn.ops(1).reg == tmp_reg && insn.ops(2).type == ARM64_OP_IMM && insn.ops(2).shift.type == ARM64_SFT_LSL) {
			// large static field offset
			tmp_reg = insn.ops(0).reg;
			load_offset = insn.ops(2).imm << insn.ops(2).shift.value;
			++insn;
		}

		if (insn.id() != ARM64_INS_STR && insn.id() != ARM64_INS_LDR) {
			FATAL("static field without STR or LDR");
		}

		INSN_ASSERT(insn.ops(1).mem.base == tmp_reg);
		load_offset |= insn.ops(1).mem.disp;
		const auto field_offset = load_offset >> 1;

		if (insn.id() == ARM64_INS_STR) {
			const auto reg = A64::Register{ insn.ops(0).reg };
			++insn;
			return std::make_unique<StoreStaticFieldInstr>(insn.Wrap(marker.Take()), reg, field_offset);
		}
		else {
			INSN_ASSERT(insn.ops(0).reg == result_reg);
			++insn;

			// load Sentinel from PP to check if this field is initialized?
			const auto objPoolInstr = getObjectPoolInstruction(insn);
			if (!objPoolInstr.IsSet() || objPoolInstr.dstReg != A64::TMP_REG || objPoolInstr.item.ValueTypeId() != dart::kSentinelCid) {
				// this is just load static field (same as global variable). cannot find the owner
				return std::make_unique<LoadStaticFieldInstr>(insn.Wrap(marker.Take()), dstReg, field_offset);
			}

			// save current instruction to move forward and check if there are extra instructions for checking
			const auto loadStaicInstr_endIns = insn.Current();

			// if any condition is not met, fallback to just load static field
			INSN_ASSERT(insn.id() == ARM64_INS_CMP);
			INSN_ASSERT(A64::Register{ insn.ops(0).reg } == dstReg);
			INSN_ASSERT(A64::Register{ insn.ops(1).reg } == A64::TMP_REG);
			++insn;

			INSN_ASSERT(insn.id() == ARM64_INS_B);
			if (insn.cc() == ARM64_CC_NE) {
				const auto cont_addr = insn.ops(0).imm;
				++insn;

				// get pool object
				const auto objPoolInstr = getObjectPoolInstruction(insn);
				if (objPoolInstr.IsSet()) {
					INSN_ASSERT(objPoolInstr.dstReg == A64::Register{ dart::InitStaticFieldABI::kFieldReg });
					INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kFieldCid);
					auto& dartField = objPoolInstr.item.Get<VarField>()->field;
					INSN_ASSERT(dartField.Offset() == field_offset);

					// GenerateStubCall()
					// - EmitCallToStub() might be GenerateUnRelocatedPcRelativeCall() or BranchLink()
					if (insn.id() == ARM64_INS_BL) {
						auto dartFn = app.GetFunction(insn.ops(0).imm);
						ASSERT(dartFn->IsStub());
						const auto stubKind = reinterpret_cast<DartStub*>(dartFn)->kind;
						INSN_ASSERT(stubKind == DartStub::InitLateStaticFieldStub || stubKind == DartStub::InitLateFinalStaticFieldStub);
						++insn;
					}
					else {
						// BranchLink()
						const auto objPoolInstr = getObjectPoolInstruction(insn);
						INSN_ASSERT(objPoolInstr.IsSet() && objPoolInstr.dstReg == A64::Register{ dart::CODE_REG });
						// TODO: what kind of object is loaded?

						INSN_ASSERT(insn.id() == ARM64_INS_LDUR);
						INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
						INSN_ASSERT(insn.ops(1).mem.base == ToCapstoneReg(dart::CODE_REG));
						INSN_ASSERT(insn.ops(1).mem.disp == dart::Code::entry_point_offset(dart::CodeEntryKind::kNormal) - dart::kHeapObjectTag);
						++insn;

						INSN_ASSERT(insn.id() == ARM64_INS_BLR);
						INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
						++insn;
					}
					INSN_ASSERT(insn.address() == cont_addr);

					return std::make_unique<InitLateStaticFieldInstr>(insn.Wrap(marker.Take()), dstReg, dartField);
				}
			}
			else {
				// late intialize error
				INSN_ASSERT(insn.cc() == ARM64_CC_EQ);
				const auto error_addr = insn.ops(0).imm;
				INSN_ASSERT(error_addr > insn.NextAddress() && error_addr < dartFn->AddressEnd());
				++insn;

				auto insn2 = AsmIterator(insn, error_addr);
				const auto objPoolInstr = getObjectPoolInstruction(insn2);
				if (!objPoolInstr.IsSet() && objPoolInstr.dstReg == A64::Register{dart::LateInitializationErrorABI::kFieldReg} && objPoolInstr.item.ValueTypeId() == dart::kFieldCid) {
					auto& dartField = objPoolInstr.item.Get<VarField>()->field;
					INSN_ASSERT(dartField.Offset() == field_offset);
					insn2.Next();

					INSN_ASSERT(insn2.id() == ARM64_INS_BL && insn2.ops(0).type == ARM64_OP_IMM);
					auto fn = app.GetFunction(insn2.ops(0).imm);
					auto stub = fn->AsStub();
					INSN_ASSERT(stub->kind == DartStub::LateInitializationErrorSharedWithoutFPURegsStub || stub->kind == DartStub::LateInitializationErrorSharedWithFPURegsStub);
					insn2.Next();
					// load static field but throw an exception if it is not initialized
					return std::make_unique<LoadStaticFieldInstr>(insn.Wrap(marker.Take()), dstReg, field_offset);
				}
			}

			// this one might be for if/else when Static field is not initialized
			insn.SetCurrent(loadStaicInstr_endIns);
			return std::make_unique<LoadStaticFieldInstr>(insn.Wrap(marker.Take()), dstReg, field_offset);
		}
	}

	return nullptr;
}

std::unique_ptr<AllocateObjectInstr> FunctionAnalyzer::processTryAllocateObject(AsmIterator& insn)
{
	// Assembler::TryAllocateObject() with inline_alloc
	//   example of allocate double (0x10 is size)
	// ; make sure Thread stack has enough space
	// 0x21c66c: ldp  x0, x1, [x26, #0x50]  (Thread::top)
	// 0x21c670: add  x0, x0, #0x10
	// 0x21c674: cmp  x1, x0
	// 0x21c678: b.ls  #0x21c894  ; jump to slow path. AllocateDouble() will be called
	// 0x21c67c: str  x0, [x26, #0x50]  (Thread::top)
	// 0x21c680: sub  x0, x0, #0xf
	// 0x21c684: movz  x1, #0xe148
	// 0x21c688: movk  x1, #0x3, lsl #16
	// 0x21c68c: stur  x1, [x0, #-1]
	// ...
	// 0x21c894: str  q0, [x15, #-0x10]!  ; save q0
	// 0x21c898: stp  x2, x3, [x15, #-0x10]!  ; save x2, x3
	// 0x21c89c: bl  #0x2e511c  # AllocateDoubleStub
	// 0x21c8a0: ldp  x2, x3, [x15], #0x10  ; restore x2, x3
	// 0x21c8a4: ldr  q0, [x15], #0x10  ; restore q0
	// 0x21c8a8: b  #0x21c690
	if (insn.id() == ARM64_INS_LDP && insn.ops(2).mem.base == CSREG_DART_THR && insn.ops(2).mem.disp == dart::Thread::top_offset()) {
		const auto inst_reg = insn.ops(0).reg;
		const auto temp_reg = insn.ops(1).reg;
		InsnMarker marker(insn);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_ADD);
		INSN_ASSERT(insn.ops(0).reg == inst_reg);
		INSN_ASSERT(insn.ops(1).reg == inst_reg);
		const auto inst_size = insn.ops(2).imm;
		INSN_ASSERT(inst_size == 0x10);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_CMP);
		INSN_ASSERT(insn.ops(0).reg == temp_reg);
		INSN_ASSERT(insn.ops(1).reg == inst_reg);
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_B && insn.cc() == ARM64_CC_LS);
		const uint64_t slow_path = (uint64_t)insn.ops(0).imm;
		// slow path is always at the end of function
		INSN_ASSERT(insn.address() < slow_path && slow_path < dartFn->AddressEnd());
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_STR);
		INSN_ASSERT(insn.ops(0).reg == inst_reg);
		INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_THR && insn.ops(1).mem.disp == dart::Thread::top_offset());
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_SUB);
		INSN_ASSERT(insn.ops(0).reg == inst_reg);
		INSN_ASSERT(insn.ops(1).reg == inst_reg);
		INSN_ASSERT(insn.ops(2).imm == inst_size - 1); // -1 because of kHeapObjectTag
		++insn;

		// also can reuse loadImm
		INSN_ASSERT(insn.IsMovz());
		INSN_ASSERT(insn.ops(0).reg == temp_reg);
		auto tag = insn.ops(1).imm;
		++insn;

		// movk instruction is needed with bits 16-32 is not zero (class id > 16)
		if (insn.id() == ARM64_INS_MOVK) {
			INSN_ASSERT(insn.ops(0).reg == temp_reg);
			INSN_ASSERT(insn.ops(1).shift.value == 16);
			tag |= (insn.ops(1).imm << 16);
			++insn;
		}

		INSN_ASSERT(insn.id() == ARM64_INS_STUR);
		INSN_ASSERT(insn.ops(0).reg == temp_reg);
		INSN_ASSERT(insn.ops(1).mem.base == inst_reg && insn.ops(1).mem.disp == -1); // because of kHeapObjectTag
		++insn;

		const uint32_t cid = (tag >> dart::UntaggedObject::kClassIdTagPos) & ((1 << dart::UntaggedObject::kClassIdTagSize) - 1);
		auto dartCls = app.GetClass(cid);
		//INSN_ASSERT(dartCls->Size() < 0 || dartCls->Size() == inst_size);

		const auto dstReg = A64::Register{ inst_reg };

		// TODO: extra check by walk through slow path

		return std::make_unique<AllocateObjectInstr>(insn.Wrap(marker.Take()), dstReg, *dartCls);
	}

	return nullptr;
}

std::unique_ptr<WriteBarrierInstr> FunctionAnalyzer::processWriteBarrierInstr(AsmIterator& insn)
{
	// In Assembler::StoreBarrier() and Assembler::StoreIntoArrayBarrier()
	// if (can_be_smi == kValueCanBeSmi) {
	//     BranchIfSmi(value, &done);
	// }
	// 
	// 0x2a766c: tbz  w0, #0, #0x2a7688  ; BranchIfSmi()
	// 0x2a7670: ldurb  w16, [x1, #-1]
	// 0x2a7674: ldurb  w17, [x0, #-1]
	// 0x2a7678: and  x16, x17, x16, lsr #2
	// 0x2a767c: tst  x16, x28, lsr #32
	// 0x2a7680: b.eq  #0x2a7688
	// 0x2a7684: bl  #0x44bdf0  # ArrayWriteBarrierStub
	//const auto objReg = A64::FromDartReg(dart::kWriteBarrierObjectReg);
	//const auto valReg = A64::FromDartReg(dart::kWriteBarrierValueReg);
	A64::Register objReg;
	A64::Register valReg;

	InsnMarker marker(insn);

	uint64_t contSmiAddr = 0;
	if (insn.id() == ARM64_INS_TBZ) {
		if (insn.ops(1).imm != dart::kSmiTag)
			return nullptr;
		valReg = A64::Register{ insn.ops(0).reg };
		contSmiAddr = (uint64_t)insn.ops(2).imm;
		++insn;
	}

	if (insn.id() != ARM64_INS_LDURB || A64::Register{ insn.ops(0).reg } != A64::TMP_REG || insn.ops(1).mem.disp != -1)
		return nullptr; // it is not WriteBarrier
	objReg = A64::Register{ insn.ops(1).mem.base };
	++insn;

	if (insn.id() != ARM64_INS_LDURB || A64::Register{ insn.ops(0).reg } != A64::TMP2_REG || insn.ops(1).mem.disp != -1)
		return nullptr;
	if (valReg != A64::Register::kNoRegister) {
		INSN_ASSERT(A64::Register{ insn.ops(1).mem.base } == valReg);
	}
	else {
		valReg = A64::Register{ insn.ops(1).mem.base };
	}
	++insn;

	INSN_ASSERT(insn.id() == ARM64_INS_AND);
	INSN_ASSERT(A64::Register{ insn.ops(0).reg } == A64::TMP_REG);
	INSN_ASSERT(A64::Register{ insn.ops(1).reg } == A64::TMP2_REG);
	INSN_ASSERT(A64::Register{ insn.ops(2).reg } == A64::TMP_REG);
	INSN_ASSERT(insn.ops(2).shift.type == ARM64_SFT_LSR && insn.ops(2).shift.value == 2);
	++insn;

	INSN_ASSERT(insn.id() == ARM64_INS_TST);
	INSN_ASSERT(insn.ops(0).reg == CSREG_DART_TMP);
	INSN_ASSERT(insn.ops(1).reg == CSREG_DART_HEAP);
	INSN_ASSERT(insn.ops(1).shift.type == ARM64_SFT_LSR && insn.ops(1).shift.value == 32);
	++insn;

	INSN_ASSERT(insn.id() == ARM64_INS_B && insn.cc() == ARM64_CC_EQ);
	const auto contAddr = (uint64_t)insn.ops(0).imm;
	INSN_ASSERT(contSmiAddr == 0 || contSmiAddr == contAddr);
	++insn;

	bool spill_lr = false;
	if (insn.id() == ARM64_INS_STR) {
		// spill_lr => Push(LR)
		// 0x1d5f58: str  x30, [x15, #-8]!
		INSN_ASSERT(insn.writeback());
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
		INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_SP && insn.ops(1).mem.disp == -8);
		spill_lr = true;
		++insn;
	}

	// TODO: call instruction is not processed. AsmText cannot show the stub name.
	bool isArray;
	if (insn.id() == ARM64_INS_BL) {
		auto stub = app.GetFunction(insn.ops(0).imm);
		INSN_ASSERT(stub->IsStub());
		const auto stubKind = reinterpret_cast<DartStub*>(stub)->kind;
		INSN_ASSERT(stubKind == DartStub::WriteBarrierWrappersStub || stubKind == DartStub::ArrayWriteBarrierStub);
		isArray = stubKind == DartStub::ArrayWriteBarrierStub;
		++insn;
	}
	else {
		// from generate_invoke_write_barrier_wrapper_ lambda function
		// - write_barrier_wrappers_thread_offset(reg) in thread.h calculates thread offset from a register
		// Call(Address target) always use LR register
		INSN_ASSERT(insn.id() == ARM64_INS_LDR);
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
		INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_THR);
		if (insn.ops(1).mem.disp == AOT_Thread_array_write_barrier_entry_point_offset) {
			isArray = true;
		}
		else {
			const auto existed = std::find(std::begin(AOT_Thread_write_barrier_wrappers_thread_offset),
				std::end(AOT_Thread_write_barrier_wrappers_thread_offset), insn.ops(1).mem.disp) != std::end(AOT_Thread_write_barrier_wrappers_thread_offset);
			INSN_ASSERT(existed);
			isArray = false;
		}
		const auto epReg = insn.ops(0).reg;
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_BLR);
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
		++insn;
	}

	if (spill_lr) {
		// 0x1d5f60: ldr  x30, [x15], #8
		INSN_ASSERT(insn.id() == ARM64_INS_LDR && insn.writeback());
		INSN_ASSERT(insn.ops(0).reg == CSREG_DART_LR);
		INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_SP);
		INSN_ASSERT(insn.ops(2).imm == 8);
		++insn;
	}

	INSN_ASSERT(insn.address() == contAddr);

	return std::make_unique<WriteBarrierInstr>(insn.Wrap(marker.Take()), objReg, valReg, isArray);
}

static ArrayOp getArrayOp(AsmIterator& insn)
{
	ArrayOp op;
	if (insn.writeback())
		return ArrayOp();
	switch (insn.id()) {
	case ARM64_INS_LDUR: {
		// for 64 bit integer, LDUR is used too
		const auto regSize = GetCsRegSize(insn.ops(0).reg);
		//if (regSize == dart::kCompressedWordSize)
		return ArrayOp(regSize, true, ArrayOp::Unknown);
		//return ArrayOp(regSize, true, ArrayOp::TypedUnknown);
	}
	case ARM64_INS_LDURSW:
		return ArrayOp(4, true, ArrayOp::TypedSigned);
	case ARM64_INS_LDRB:
		return ArrayOp(1, true, ArrayOp::TypedUnsigned);
	case ARM64_INS_LDRSB:
		return ArrayOp(1, true, ArrayOp::TypedSigned);
	case ARM64_INS_LDRH:
		return ArrayOp(2, true, ArrayOp::TypedUnsigned);
	case ARM64_INS_LDURSH:
		return ArrayOp(2, true, ArrayOp::TypedSigned);
	case ARM64_INS_STUR: {
		const auto regSize = GetCsRegSize(insn.ops(0).reg);
		//if (regSize == dart::kCompressedWordSize)
		return ArrayOp(regSize, false, ArrayOp::Unknown);
		//return ArrayOp(regSize, false, ArrayOp::TypedUnknown);
	}
	case ARM64_INS_STRB:
		return ArrayOp(1, false, ArrayOp::TypedUnknown);
	case ARM64_INS_STURH:
		// no signed/unsigned info
		return ArrayOp(2, false, ArrayOp::TypedUnknown);
	}
	return ArrayOp();
}

std::unique_ptr<ILInstr> FunctionAnalyzer::processLoadStore(AsmIterator& insn)
{
	InsnMarker marker(insn);

	// handles load/store array element by calculating offset first (index as register)
	// Dart use only ADD instruction for calculating memory offset
	if (insn.id() == ARM64_INS_ADD) {
		// tmp register for storing memory offset is TMP or R25 as kWriteBarrierSlotReg (in case of StoreBerrier is needed)
		if (insn.ops(0).reg == CSREG_DART_WB_SLOT) {
			// StoreIndexedInstr::EmitNativeCode()
			//   class_id() == kArrayCid && ShouldEmitStoreBarrier()
			// - immediate as index
			// 0x2a7664: add  x25, x1, #0x13
			// 0x2a7668: str  w0, [x25]
			// 0x2a766c: tbz  w0, #0, #0x2a7688
			// 0x2a7670: ldurb  w16, [x1, #-1]
			// 0x2a7674: ldurb  w17, [x0, #-1]
			// 0x2a7678: and  x16, x17, x16, lsr #2
			// 0x2a767c: tst  x16, x28, lsr #32
			// 0x2a7680: b.eq  #0x2a7688
			// 0x2a7684: bl  #0x44bdf0  # ArrayWriteBarrierStub
			// - register as index
			// 0x16ce18: add  x25, x1, x3, lsl #2
			// 0x16ce1c: add  x25, x25, #0xf
			// 0x16ce20: str  w0, [x25]
			if (insn.ops(1).reg != CSREG_DART_WB_OBJECT)
				return nullptr; // not array load/store

			const auto objReg = A64::Register{ dart::kWriteBarrierObjectReg };
			const auto valReg = A64::Register{ dart::kWriteBarrierValueReg };
			// TODO: objReg type MUST be Dart array
			auto idx = VarStorage::NewSmallImm(0);
			if (insn.ops(2).type == ARM64_OP_IMM) {
				// const int64_t offset = index * index_scale + HeapDataOffset(is_external, cid);
				const auto arr_idx = (insn.ops(2).imm + dart::kHeapObjectTag - dart::Array::data_offset()) / dart::kCompressedWordSize;
				if (arr_idx < 0)
					return nullptr;
				idx = VarStorage::NewSmallImm(arr_idx);
				++insn;
			}
			else {
				// register as index
				INSN_ASSERT(insn.ops(2).shift.type == ARM64_SFT_LSL &&
					(insn.ops(2).shift.value == dart::kCompressedWordSizeLog2 ||
						(insn.ops(2).shift.value == dart::kCompressedWordSizeLog2 - 1 || insn.ops(2).ext == ARM64_EXT_SXTW)));
				idx = VarStorage(A64::Register{ insn.ops(2).reg });
				++insn;

				INSN_ASSERT(insn.id() == ARM64_INS_ADD);
				INSN_ASSERT(insn.ops(0).reg == CSREG_DART_WB_SLOT);
				INSN_ASSERT(insn.ops(1).reg == CSREG_DART_WB_SLOT);
				INSN_ASSERT(insn.ops(2).imm == dart::Array::data_offset() - dart::kHeapObjectTag);
				++insn;
			}

			INSN_ASSERT(insn.id() == ARM64_INS_STR);
			INSN_ASSERT(A64::Register{ insn.ops(0).reg } == valReg && GetCsRegSize(insn.ops(0).reg) == dart::kCompressedWordSize);
			INSN_ASSERT(insn.ops(1).mem.base == CSREG_DART_WB_SLOT && insn.ops(1).mem.disp == 0);
			++insn;

			const auto il_wb = processWriteBarrierInstr(insn);
			INSN_ASSERT(il_wb);
			INSN_ASSERT(il_wb->isArray);
			INSN_ASSERT(il_wb->objReg == objReg && il_wb->valReg == valReg);

			ArrayOp arrayOp(dart::kCompressedWordSize, false, ArrayOp::List);
			return std::make_unique<StoreArrayElementInstr>(insn.Wrap(marker.Take()), valReg, objReg, idx, arrayOp);
		}

		// calculating memory offset (array index) should be from register only
		// tmp register size is always 8 (for 64 bits)
		if (insn.ops(2).type == ARM64_OP_REG && insn.ops(1).reg != CSREG_DART_FP && GetCsRegSize(insn.ops(0).reg) == 8) {
			const auto tmpReg = insn.ops(0).reg;
			// LoadIndexedInstr::EmitNativeCode() or StoreIndexedInstr::EmitNativeCode() non barrier path
			// 0x220460: add  x16, x3, x5, lsl #2
			// 0x220464: ldur  w1, [x16, #0xf]
			//  ===
			// 0x2c3958: add  x16, x2, x5
			// 0x2c395c: ldrb  w7, [x16, #0x17]  ; Uint8List
			//  ===
			// 0x40b868: add     x16, x1, w0, sxtw #1 ; w0 is Smi, so shift only 1 instead of 2
			// 0x40b86c: ldursw  x1, [x16, #0x17]  ; Int32List
			const auto arrReg = A64::Register{ insn.ops(1).reg };
			const auto idx = VarStorage(A64::Register{ insn.ops(2).reg });
			const auto shift = insn.ops(2).shift;
			if (shift.type != ARM64_SFT_INVALID && shift.type != ARM64_SFT_LSL)
				return nullptr;
			const auto ext = insn.ops(2).ext;
			++insn;

			// array offset for index as register always be start of payload
			const auto arr_data_offset = insn.ops(1).mem.disp;
			if (insn.ops(1).mem.base != tmpReg || arr_data_offset < 8)
				return nullptr;
			const auto arrayOp = getArrayOp(insn);
			if (!arrayOp.IsArrayOp())
				return nullptr;
			const auto idxShiftVal = arrayOp.SizeLog2();
			if (shift.value == idxShiftVal) {
				// nothing to do
			}
			else if (shift.value + 1 == idxShiftVal) {
				//if (idxShiftVal)
				//	INSN_ASSERT(ext == ARM64_EXT_SXTW);
				// TODO: this index is Smi
			}
			else {
				FATAL("invalid shift for array operation");
			}
			bool isTypedData = dart::UntaggedTypedData::payload_offset() - dart::kHeapObjectTag == arr_data_offset;
			INSN_ASSERT(isTypedData || arr_data_offset == dart::Array::data_offset() - dart::kHeapObjectTag);
			const auto op0Reg = A64::Register{ insn.ops(0).reg };
			++insn;
			if (arrayOp.isLoad) {
				// load always uses TMP register
				INSN_ASSERT(tmpReg == CSREG_DART_TMP);
				return std::make_unique<LoadArrayElementInstr>(insn.Wrap(marker.Take()), op0Reg, arrReg, idx, arrayOp);
			}
			else {
				// store operation
				return std::make_unique<StoreArrayElementInstr>(insn.Wrap(marker.Take()), op0Reg, arrReg, idx, arrayOp);
			}
		}
	}

	if (insn.ops(1).mem.base != CSREG_DART_FP && insn.ops(1).mem.disp != 0) {
		// load/store with fixed offset
		const auto arrayOp = getArrayOp(insn);
		if (arrayOp.IsArrayOp()) {
			const auto valReg = A64::Register{ insn.ops(0).reg };
			const auto objReg = A64::Register{ insn.ops(1).mem.base };
			const auto offset = insn.ops(1).mem.disp;
			++insn;

			if (arrayOp.arrType == ArrayOp::Unknown) {
				// might be array or object. set it as object first.
				if (arrayOp.isLoad) {
					return std::make_unique<LoadFieldInstr>(insn.Wrap(marker.Take()), valReg, objReg, offset);
				}
				else {
					const auto il_wb = processWriteBarrierInstr(insn);
					if (il_wb) {
						// with WriteBearrier, we can determine if the object is array or not
						// but it is still difficult if it is list or typed data
						INSN_ASSERT(il_wb->objReg == objReg && il_wb->valReg == valReg);
						if (il_wb->isArray)
							return std::make_unique<StoreArrayElementInstr>(insn.Wrap(marker.Take()), valReg, objReg, VarStorage::NewSmallImm(offset), arrayOp);
						else
							return std::make_unique<StoreFieldInstr>(insn.Wrap(marker.Take()), valReg, objReg, offset);
					}
					else {
						return std::make_unique<StoreFieldInstr>(insn.Wrap(marker.Take()), valReg, objReg, offset);
					}
				}
			}
			else {
				// array
				const auto idx = VarStorage::NewSmallImm((offset + dart::kHeapObjectTag - dart::UntaggedTypedData::payload_offset()) / arrayOp.size);
				if (arrayOp.isLoad) {
					return std::make_unique<LoadArrayElementInstr>(insn.Wrap(marker.Take()), valReg, objReg, idx, arrayOp);
				}
				else {
					const auto il_wb = processWriteBarrierInstr(insn);
					if (il_wb) {
						return std::make_unique<StoreArrayElementInstr>(insn.Wrap(marker.Take()), valReg, objReg, idx, arrayOp);
					}
					else {
						return std::make_unique<StoreArrayElementInstr>(insn.Wrap(marker.Take()), valReg, objReg, idx, arrayOp);
					}
				}
			}
		}
	}

	return nullptr;
}

void FunctionAnalyzer::asm2il()
{
	AsmIterator insn(asm_insns.FirstPtr(), asm_insns.LastPtr());

	handlePrologue(insn, fnInfo->asmTexts.FirstStackLimitAddress());

	do {
		bool ok = false;
		try {
			for (auto matcher : matcherFns) {
				auto il = std::invoke(matcher, this, insn);
				if (il) {
					fnInfo->AddIL(std::move(il));
					ok = true;
					break;
				}
			}
		}
		catch (InsnException& e) {
			printInsnException(e);
		}

		if (!ok) {
			// unhandle case
			auto ins = insn.Current();
			fnInfo->AddIL(std::make_unique<UnknownInstr>(ins, fnInfo->asmTexts.AtAddr(ins->address)));
			++insn;
		}
	} while (!insn.IsEnd());
}

void CodeAnalyzer::asm2il(DartFunction* dartFn, AsmInstructions& asm_insns)
{
	FunctionAnalyzer analyzer{ dartFn->GetAnalyzedData(), dartFn, asm_insns, app };
	analyzer.asm2il();
}
	
AsmTexts CodeAnalyzer::convertAsm(AsmInstructions& asm_insns)
{
	// convert register name in op_str
	std::vector<AsmText> asm_texts(asm_insns.Count());
	uint64_t first_stack_limit_addr = 0;
	int max_param_stack_offset = 0;

	for (size_t i = 0; i < asm_insns.Count(); i++) {
		auto insn = asm_insns.Ptr(i);
		auto& text_asm = asm_texts.at(i);

		text_asm.addr = insn->address;
		text_asm.dataType = AsmText::None;
		
		memset(text_asm.text, ' ', 16);
		memcpy(text_asm.text, insn->mnemonic, strlen(insn->mnemonic));
		auto ptr = text_asm.text + 16;
		auto op_ptr = insn->op_str;
		bool token_start = true;
		while (*op_ptr != '\0') {
			if (token_start) { 
				if (op_ptr[0] == 'x' || op_ptr[0] == 'w') {
					bool do_replacement = true;
					if (op_ptr[1] == '1' && op_ptr[2] == '5') {
						*ptr++ = 'S';
						*ptr++ = 'P';
					}
					else if (op_ptr[1] == '2' && op_ptr[2] == '2') {
						*ptr++ = 'N';
						*ptr++ = 'U';
						*ptr++ = 'L';
						*ptr++ = 'L';
					}
					else if (op_ptr[1] == '2' && op_ptr[2] == '6') {
						*ptr++ = 'T';
						*ptr++ = 'H';
						*ptr++ = 'R';
						// Normally thread access is part of instructions. we don't need this case for translating to IL.
						// but the thread offset information is nice to have in assembly
						// thread access always be the last operand
						auto& op = insn->detail->arm64.operands[insn->detail->arm64.op_count - 1];
						if (op.type == ARM64_OP_MEM && op.mem.base == CSREG_DART_THR) {
							text_asm.threadOffset = op.mem.disp;
							text_asm.dataType = AsmText::ThreadOffset;
							if (first_stack_limit_addr == 0 && op.mem.disp == AOT_Thread_stack_limit_offset) {
								// mark it as end of prologue
								first_stack_limit_addr = insn->address;
							}
						}
					}
					else if (op_ptr[1] == '2' && op_ptr[2] == '7') {
						*ptr++ = 'P';
						*ptr++ = 'P';
					}
					else if (op_ptr[1] == '2' && op_ptr[2] == '8') {
						*ptr++ = 'H';
						*ptr++ = 'E';
						*ptr++ = 'A';
						*ptr++ = 'P';
					}
					else if (op_ptr[1] == '2' && op_ptr[2] == '9') {
						*ptr++ = 'f';
						*ptr++ = 'p';
						// save maximum stack offset for accessing parameter
						if (insn->id == ARM64_INS_LDR) {
							auto& op = insn->detail->arm64.operands[1];
							if (op.mem.base == CSREG_DART_FP && op.mem.disp > max_param_stack_offset) {
								max_param_stack_offset = op.mem.disp;
							}
						}
					}
					else if (op_ptr[1] == '3' && op_ptr[2] == '0') {
						*ptr++ = 'l';
						*ptr++ = 'r';
					}
					else {
						do_replacement = false;
					}

					if (do_replacement) {
						op_ptr += 3;
						continue;
					}
				}
			}
			switch (*op_ptr) {
			case ' ':
			case '[':
				token_start = true;
				break;
			default:
				token_start = false;
				break;
			}
			*ptr++ = *op_ptr++;
		}
		*ptr = '\0';
	}

	return AsmTexts{ asm_texts, first_stack_limit_addr, max_param_stack_offset };
}

#endif // NO_CODE_ANALYSIS
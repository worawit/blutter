#include "pch.h"
#include "CodeAnalyzer.h"
#include "DartApp.h"
#include "VarValue.h"
#include <source_location>

class InsnException
{
public:
	explicit InsnException(const char* cond, cs_insn* ins, const std::source_location& location = std::source_location::current())
		: cond{ cond }, ins{ ins }, location{ location } {}

	std::string cond;
	cs_insn* ins;
	std::source_location location;
};

#define INSN_ASSERT(cond) \
  do {                    \
	if (!(cond)) throw InsnException(#cond, insn.ptr()); \
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
			return new VarStub(*stub->AsStub());
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
		case dart::kInstanceCid:
			return new VarInstance(app.GetClass(dart::kInstanceCid));
		}

		if (obj.IsInstance()) {
			auto dartCls = app.GetClass(obj.GetClassId());
			RELEASE_ASSERT(dartCls->Id() >= dart::kNumPredefinedCids);
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

struct ObjectPoolInstr {
	int insCnt{ 0 };
	A64::Register dstReg;
	VarItem item{};
};

static ObjectPoolInstr processGetObjectPoolInstruction(DartApp& app, AsmInstruction insn)
{
	int64_t offset = 0;
	int insCnt = 0;
	A64::Register dstReg;
	//auto ins_ptr = insn.ptr();
	if (insn.id() == ARM64_INS_LDR) {
		// PP offset less than 12 bits
		if (insn.ops[1].mem.base == CSREG_DART_PP && insn.ops[1].mem.index == ARM64_REG_INVALID) {
			offset = insn.ops[1].mem.disp;
			dstReg = A64::Register{ insn.ops[0].reg };
			insCnt = 1;
		}
	}
	else if (insn.id() == ARM64_INS_ADD) {
		// special case from ARM64 because true and false are loaded from NULL instead of Object Pool
		if (insn.ops[1].reg == CSREG_DART_NULL) {
			// In Assembler::LoadObjectHelper() of arm64 only. load true and false from null
			ASSERT(insn.ops[2].type == ARM64_OP_IMM);
			const auto offset = insn.ops[2].imm;
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
			dstReg = A64::Register{ insn.ops[0].reg };
			return { 1, dstReg, VarItem{ VarStorage::NewImmediate(), new VarBoolean(val) } };
		}

		if (insn.ops[1].reg == CSREG_DART_PP && insn.ops[2].type == ARM64_OP_IMM && insn.ops[2].shift.type == ARM64_SFT_LSL && insn.ops[2].shift.value == 12) {
			auto base = insn.ops[2].imm << 12;
			auto offset_reg = insn.ops[0].reg;

			++insn;
			if (insn.id() == ARM64_INS_LDR) {
				INSN_ASSERT(insn.ops[1].mem.base == offset_reg);
				offset = base + insn.ops[1].mem.disp;
			}
			else if (insn.id() == ARM64_INS_LDP) {
				INSN_ASSERT(insn.ops[2].mem.base == offset_reg);
				offset = base + insn.ops[2].mem.disp;
			}
			else if (insn.id() == ARM64_INS_ADD) {
				// use when loading pair from object pool (local 2 entries)
				// see it for UnlinkedCall by the next entry is the jump address
				INSN_ASSERT(insn.ops[2].type == ARM64_OP_IMM);
				offset = base + insn.ops[2].imm;
			}
			dstReg = A64::Register{ insn.ops[0].reg };
			insCnt = 2;
		}
	}
	else if (insn.id() == ARM64_INS_MOV) {
		// more than 20 bits (never seen it)
		// MOV X5, #offset_low
		// MOVK X5, #offset_high LSL#16
		// LDR X6, [X27,X5]
		const auto offset_reg = insn.ops[0].reg;
		offset = insn.ops[1].imm;

		++insn;
		if (insn.id() == ARM64_INS_MOVK && insn.ops[0].reg == offset_reg && 
			insn.ops[1].type == ARM64_OP_IMM && insn.ops[1].shift.type == ARM64_SFT_LSL && insn.ops[1].shift.value == 16)
		{
			offset |= insn.ops[1].imm << 16;

			++insn;
			if (insn.id() == ARM64_INS_LDR && insn.ops[1].mem.base == CSREG_DART_PP && insn.ops[1].mem.index == offset_reg) {
				dstReg = A64::Register{ insn.ops[0].reg };
				insCnt = 3;
			}
		}
	}

	if (insCnt == 0) {
		return ObjectPoolInstr{};
	}

	auto val = getPoolObject(app, offset, dstReg);
	return ObjectPoolInstr{ insCnt, dstReg, VarItem{VarStorage::NewPool((int)offset), val} };
}

struct ILResult {
	cs_insn* lastIns{ nullptr };
	std::unique_ptr<ILInstr> il;
	uint64_t NextAddress() const { return lastIns->address + lastIns->size; }
	// TODO: ASSERT(il->Kind())
	template <typename T, typename = std::enable_if<std::is_base_of<ILInstr, T>::value>>
	T* get() { return reinterpret_cast<T*>(il.get()); }
};
struct ILWBResult {
	cs_insn* lastIns{ nullptr };
	std::unique_ptr<WriteBarrierInstr> il;
	uint64_t NextAddress() const { return lastIns->address + lastIns->size; }
};
class FunctionAnalyzer
{
public:
	FunctionAnalyzer(AnalyzedFnData* fnInfo, DartFunction* dartFn, AsmInstructions& asm_insns, DartApp& app)
		: fnInfo{ fnInfo }, dartFn{ dartFn }, asm_insns{ asm_insns }, app{ app } {}

	void asm2il();

	ILResult processEnterFrameInstr(AsmInstruction insn);
	ILResult processLeaveFrameInstr(AsmInstruction insn);
	ILResult processAllocateStackInstr(AsmInstruction insn);
	ILResult processCheckStackOverflowInstr(AsmInstruction insn);
	ILResult processLoadValueInstr(AsmInstruction insn);
	ILResult processDecompressPointerInstr(AsmInstruction insn);
	ILResult processOptionalParametersInstr(AsmInstruction insn);
	ILResult processSaveRegisterInstr(AsmInstruction insn);
	ILResult processLoadSavedRegisterInstr(AsmInstruction insn);
	ILResult processCallInstr(AsmInstruction insn);
	ILResult processGdtCallInstr(AsmInstruction insn);
	ILResult processReturnInstr(AsmInstruction insn);
	ILResult processInstanceofNoTypeArgumentInstr(AsmInstruction insn);
	ILResult processLoadImmInstr(AsmInstruction insn);
	ILResult processBranchIfSmiInstr(AsmInstruction insn);
	ILResult processLoadClassIdInstr(AsmInstruction insn);
	ILResult processBoxInt64Instr(AsmInstruction insn);
	ILResult processLoadInt32FromBoxOrSmiInstr(AsmInstruction insn);
	ILResult processLoadTaggedClassIdMayBeSmiInstr(AsmInstruction insn);
	ILResult processLoadFieldTableInstr(AsmInstruction insn);
	ILResult processTryAllocateObject(AsmInstruction insn);
	ILWBResult processWriteBarrier(AsmInstruction insn);
	ILResult processWriteBarrierInstr(AsmInstruction insn);
	ILResult processLoadStore(AsmInstruction insn);

	AnalyzedFnData* fnInfo;
	DartFunction* dartFn;
	AsmInstructions& asm_insns;
	DartApp& app;
};
typedef ILResult(FunctionAnalyzer::* AsmMatcherFn)(AsmInstruction insn);
static const AsmMatcherFn matcherFns[] = {
	&FunctionAnalyzer::processEnterFrameInstr,
	&FunctionAnalyzer::processLeaveFrameInstr,
	&FunctionAnalyzer::processAllocateStackInstr,
	&FunctionAnalyzer::processCheckStackOverflowInstr,
	&FunctionAnalyzer::processLoadValueInstr,
	&FunctionAnalyzer::processDecompressPointerInstr,
	&FunctionAnalyzer::processOptionalParametersInstr,
	&FunctionAnalyzer::processSaveRegisterInstr,
	&FunctionAnalyzer::processLoadSavedRegisterInstr,
	&FunctionAnalyzer::processCallInstr,
	&FunctionAnalyzer::processGdtCallInstr,
	&FunctionAnalyzer::processReturnInstr,
	&FunctionAnalyzer::processInstanceofNoTypeArgumentInstr,
	//&FunctionAnalyzer::processLoadImmInstr,
	&FunctionAnalyzer::processBranchIfSmiInstr,
	&FunctionAnalyzer::processLoadClassIdInstr,
	&FunctionAnalyzer::processBoxInt64Instr,
	&FunctionAnalyzer::processLoadInt32FromBoxOrSmiInstr,
	&FunctionAnalyzer::processLoadTaggedClassIdMayBeSmiInstr,
	&FunctionAnalyzer::processLoadFieldTableInstr,
	&FunctionAnalyzer::processTryAllocateObject,
	&FunctionAnalyzer::processWriteBarrierInstr,
	&FunctionAnalyzer::processLoadStore,
};

ILResult FunctionAnalyzer::processEnterFrameInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_STP) {
		if (insn.ops[0].reg == CSREG_DART_FP && insn.ops[1].reg == ARM64_REG_LR && insn.ops[2].mem.base == CSREG_DART_SP) {
			INSN_ASSERT(insn.writeback());
			const auto insn0_addr = insn.address();
			++insn;
			INSN_ASSERT(insn.id() == ARM64_INS_MOV);
			INSN_ASSERT(insn.ops[0].reg == CSREG_DART_FP);
			INSN_ASSERT(insn.ops[1].reg == CSREG_DART_SP);
			fnInfo->useFramePointer = true;
			return ILResult{ insn.ptr(), std::make_unique<EnterFrameInstr>(AddrRange(insn0_addr, insn.NextAddress())) };
		}
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processLeaveFrameInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_MOV) {
		if (insn.ops[0].reg == CSREG_DART_SP && insn.ops[1].reg == CSREG_DART_FP) {
			INSN_ASSERT(fnInfo->useFramePointer);
			const auto insn0_addr = insn.address();
			++insn;
			INSN_ASSERT(insn.id() == ARM64_INS_LDP && insn.op_count() == 4);
			INSN_ASSERT(insn.ops[0].reg == CSREG_DART_FP);
			INSN_ASSERT(insn.ops[1].reg == ARM64_REG_LR);
			INSN_ASSERT(insn.ops[2].mem.base == CSREG_DART_SP);
			INSN_ASSERT(insn.ops[3].imm == 0x10);
			return ILResult{ insn.ptr(), std::make_unique<LeaveFrameInstr>(AddrRange(insn0_addr, insn.NextAddress())) };
		}
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processAllocateStackInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_SUB) {
		if (insn.ops[0].reg == CSREG_DART_SP && insn.ops[1].reg == CSREG_DART_SP && insn.ops[2].type == ARM64_OP_IMM) {
			const auto stackSize = (uint32_t)insn.ops[2].imm;
			fnInfo->stackSize = stackSize;
			return ILResult{ insn.ptr(), std::make_unique<AllocateStackInstr>(insn.ptr(), stackSize) };
		}
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processCheckStackOverflowInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_LDR) {
		if (insn.ops[1].mem.base == CSREG_DART_THR && insn.ops[1].mem.disp == AOT_Thread_stack_limit_offset) {
			INSN_ASSERT(insn.ops[0].reg == CSREG_DART_TMP);
			const auto insn0_addr = insn.address();

			++insn;
			// cmp SP, TMP
			INSN_ASSERT(insn.id() == ARM64_INS_CMP);
			INSN_ASSERT(insn.ops[0].reg == CSREG_DART_SP);
			INSN_ASSERT(insn.ops[1].reg == CSREG_DART_TMP);

			++insn;
			INSN_ASSERT(insn.id() == ARM64_INS_B);
			INSN_ASSERT(insn.ops[0].type == ARM64_OP_IMM);
			uint64_t target = 0;
			// b.ls #overflow_target
			if (insn.cc() == ARM64_CC_LS) {
				target = (uint64_t)insn.ops[0].imm;
			}
			else if (insn.cc() == ARM64_CC_HI) {
				const auto cont_target = insn.ops[0].imm;

				++insn;
				INSN_ASSERT(insn.IsBranch());
				INSN_ASSERT(insn.NextAddress() == cont_target);
				target = (uint64_t)insn.ops[0].imm;
			}
			else {
				FATAL("unexpect branch condition for CheckStackOverflow");
			}

			if (target != 0) {
				// the dart compiler always put slow path at the end of function after "ret"
				INSN_ASSERT(target < dartFn->AddressEnd() && target >= insn.address());
				return ILResult{ insn.ptr(), std::make_unique<CheckStackOverflowInstr>(AddrRange(insn0_addr, insn.NextAddress()), target) };
			}
		}
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processLoadValueInstr(AsmInstruction insn)
{
	const auto insn0_addr = insn.address();
	auto objPoolInstr = processGetObjectPoolInstruction(app, insn);
	if (objPoolInstr.insCnt > 0) {
		insn += objPoolInstr.insCnt - 1;
		// TODO: load object might be start of Dart instruction, check next instruction
		return ILResult{ insn.ptr(), std::make_unique<LoadValueInstr>(AddrRange(insn0_addr, insn.NextAddress()), objPoolInstr.dstReg, objPoolInstr.item) };
	}

	int64_t imm = 0;
	A64::Register dstReg;
	// libcapstone5 use MOV instead of MOVZ
	if (insn.IsMovz()) {
		imm = insn.ops[1].imm;
		const auto tmpReg = insn.ops[0].reg;

		++insn;
		if (insn.id() == ARM64_INS_MOVK && insn.ops[0].reg == tmpReg && insn.ops[1].shift.value == 16) {
			imm |= insn.ops[1].imm << 16;
		}
		else {
			--insn;
		}
		dstReg = tmpReg;
	}
	else if (insn.id() == ARM64_INS_MOV && insn.ops[1].reg == CSREG_DART_NULL) {
		auto item = VarItem{ VarStorage::Immediate, new VarNull() };
		return ILResult{ insn.ptr(), std::make_unique<LoadValueInstr>(AddrRange(insn0_addr, insn.NextAddress()), A64::Register{ insn.ops[0].reg }, item) };
	}
	else if (insn.id() == ARM64_INS_ORR && insn.ops[1].reg == ARM64_REG_XZR && insn.ops[2].type == ARM64_OP_IMM) {
		imm = insn.ops[2].imm;
		dstReg = insn.ops[0].reg;
	}
	else if (insn.id() == ARM64_INS_MOVN) {
		imm = insn.ops[1].imm << insn.ops[1].shift.value;
		imm = ~imm;
		dstReg = insn.ops[0].reg;
	}
	else if (insn.id() == ARM64_INS_EOR && insn.ops[0].reg == insn.ops[1].reg && insn.ops[0].reg == insn.ops[2].reg) {
		dstReg = insn.ops[0].reg;
		if (dstReg.IsDecimal()) {
			auto item = VarItem{ VarStorage::Immediate, new VarDouble{0, VarValue::NativeDouble} };
			return ILResult{ insn.ptr(), std::make_unique<LoadValueInstr>(AddrRange(insn0_addr, insn.NextAddress()), dstReg, item) };
		}
	}
	else if (insn.id() == ARM64_INS_FMOV) {
		auto item = VarItem{ VarStorage::Immediate, new VarDouble{insn.ops[1].fp, VarValue::NativeDouble} };
		return ILResult{ insn.ptr(), std::make_unique<LoadValueInstr>(AddrRange(insn0_addr, insn.NextAddress()), A64::Register{ insn.ops[0].reg }, item) };
	}

	if (dstReg.IsSet()) {
		auto item = VarItem{ VarStorage::Immediate, new VarInteger{imm, VarValue::NativeInt} };
		return ILResult{ insn.ptr(), std::make_unique<LoadValueInstr>(AddrRange(insn0_addr, insn.NextAddress()), A64::Register{ dstReg }, item) };
	}

	return ILResult{};
}

ILResult FunctionAnalyzer::processDecompressPointerInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_ADD) {
		if (insn.ops[2].reg == CSREG_DART_HEAP && insn.ops[2].shift.value == 32) {
			INSN_ASSERT(insn.ops[0].reg == insn.ops[1].reg);
			return ILResult{ insn.ptr(), std::make_unique<DecompressPointerInstr>(insn.ptr(), A64::Register{ insn.ops[0].reg }) };
		}
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processOptionalParametersInstr(AsmInstruction insn)
{
	// PrologueBuilder::BuildParameterHandling()
	// it is used only when there is/are optional parameter(s) in a function arguments
	if (insn.id() == ARM64_INS_MOV && insn.ops[1].reg == CSREG_ARGS_DESC && insn.ops[0].reg == ARM64_REG_X0 && fnInfo->stackSize > 0) {
		const auto insn0_addr = insn.address();
		const auto argsDescReg = insn.ops[0].reg;
		++insn;

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
		if (insn.id() == ARM64_INS_LDUR && ToCapstoneReg(insn.ops[0].reg) == ARM64_REG_X1 && 
			insn.ops[1].mem.base == ARM64_REG_X0 && 
			insn.ops[1].mem.disp == AOT_ArgumentsDescriptor_count_offset - dart::kHeapObjectTag)
		{
			const auto paramCntReg = ToCapstoneReg(insn.ops[0].reg);
			++insn;

			// the value is Smi object. so decompress pointer instruction is generated automatically even it is not needed.
			INSN_ASSERT(insn.id() == ARM64_INS_ADD);
			INSN_ASSERT(insn.ops[0].reg == insn.ops[1].reg && insn.ops[0].reg == paramCntReg);
			INSN_ASSERT(insn.ops[2].reg == CSREG_DART_HEAP && insn.ops[2].shift.value == 32);
			++insn;

			ASSERT(fnInfo->useFramePointer);

			// == positional (fixed) parameters
			// if there is a positional parameter, the number of optional parameters is calculated first.
			const auto& [ posParamCnt, optionalParamCntReg ] = [&]() -> std::tuple<int, arm64_reg> {
				if (insn.id() == ARM64_INS_SUB && insn.ops[1].reg == ARM64_REG_X1)
					return { (int)insn.ops[2].imm >> dart::kSmiTagShift, insn.ops[0].reg };
				return { 0, ARM64_REG_INVALID };
			}();

			// unbox parameters (int, double) to native value
			const auto unboxParam = [&](A64::Register expectedSrcReg = A64::Register{}) -> std::tuple<A64::Register, A64::Register> {
				A64::Register srcReg;
				A64::Register dstReg;

				//dart::Double::value_offset();
				if (insn.id() == ARM64_INS_LDUR && insn.ops[1].mem.disp == AOT_Double_value_offset - dart::kHeapObjectTag) {
					// extract value from Double object
					dstReg = A64::Register{ insn.ops[0].reg };
					if (!dstReg.IsDecimal())
						return { A64::Register{}, A64::Register{} };
					srcReg = A64::Register{ insn.ops[1].mem.base };
					if (expectedSrcReg.IsSet() && expectedSrcReg != srcReg)
						return { A64::Register{}, A64::Register{} };
					++insn;
				}
				else {
					auto ilres = processLoadInt32FromBoxOrSmiInstr(insn);
					if (!ilres.lastIns)
						return { A64::Register{}, A64::Register{} };
					// unbox from srcReg to dstReg
					auto il = ilres.get<LoadInt32Instr>();
					srcReg = il->srcObjReg;
					if (expectedSrcReg.IsSet() && expectedSrcReg != srcReg)
						return { A64::Register{}, A64::Register{} };
					dstReg = il->dstReg;
					insn = ilres.lastIns + 1;
				}

				return { dstReg, srcReg };
			};

			if (posParamCnt > 0) {
				++insn;

				// to get the positional parameter, the pointer is calculated from FP by skipping all optional parameter (with previous calculation)
				// then, load the parameter with LDR instruction with offset same as normal function parameter.
				for (auto i = 0; i < posParamCnt; i++) {
					// don't know why some function has only one fixed positional param but the value is 2
					if (insn.id() != ARM64_INS_ADD)
						break;
					INSN_ASSERT(insn.ops[1].reg == CSREG_DART_FP);
					// shift only 2 because the number of parameter is Smi (tagged)
					INSN_ASSERT(ToCapstoneReg(insn.ops[2].reg) == optionalParamCntReg && insn.ops[2].ext == ARM64_EXT_SXTW && insn.ops[2].shift.value == 2);
					const auto tmpReg = insn.ops[0].reg;
					++insn;

					INSN_ASSERT(insn.id() == ARM64_INS_LDR);
					//INSN_ASSERT(insn.ops[0].reg == tmpReg); // ops[0] might be decimal
					// because the number of positional param might be wrong. offset cannot be calcualted.
					INSN_ASSERT(insn.ops[1].mem.base == tmpReg);//&& insn.ops[1].mem.disp == (posParamCnt - i - 1 + 2) * sizeof(void*)); // +2 for saved fp,lr
					const auto valReg = insn.ops[0].reg;
					++insn;

					// the parameter value might be saved to stack as if it is a local variable but with fixed negative offset from FP
					int32_t local_offset = 0;
					if (insn.id() == ARM64_INS_STUR) {
						INSN_ASSERT(insn.ops[0].reg == valReg);
						INSN_ASSERT(insn.ops[1].mem.base == CSREG_DART_FP);
						local_offset = insn.ops[1].mem.disp;
						++insn;
					}

					// replace the valReg of previous parameter if found
					fnInfo->params.movValReg(A64::Register{}, valReg);
					// add parameter
					fnInfo->params.params.push_back(FnParamInfo{ valReg, local_offset });
				}

				fnInfo->params.numFixedParam = fnInfo->params.params.size();
			}

			// optional positional parameter
			// test if it is optional positional parameter
			if (insn.id() == ARM64_INS_CMP) {
				const auto paramShiftIdxReg = posParamCnt > 0 ? optionalParamCntReg : paramCntReg;

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
					INSN_ASSERT(ToCapstoneReg(insn.ops[0].reg) == paramShiftIdxReg);
					INSN_ASSERT(insn.ops[1].imm == (i + 1) << 1);
					++insn;

					if (insn.IsBranch(ARM64_CC_GE)) {
						// special case. parameters are not used.
						const auto defaultValueTarget = insn.ops[0].imm;
						INSN_ASSERT(defaultValueTarget == insn.NextAddress());
						for (const auto missingTarget : missingBranchTargets) {
							INSN_ASSERT(missingTarget == defaultValueTarget);
						}
						missingBranchTargets.clear();
						++insn;
						break;
					}

					INSN_ASSERT(insn.IsBranch(ARM64_CC_LT));
					const auto defaultValueTarget = insn.ops[0].imm;
					missingBranchTargets.push_back(defaultValueTarget);
					++insn;

					// parameter might not be used and no loading value
					if (insn.id() == ARM64_INS_ADD && insn.ops[1].reg == CSREG_DART_FP) {
						// shift only 2 because the number of parameter is Smi (tagged)
						INSN_ASSERT(ToCapstoneReg(insn.ops[2].reg) == paramShiftIdxReg && insn.ops[2].ext == ARM64_EXT_SXTW && insn.ops[2].shift.value == 2);
						const auto tmpReg = insn.ops[0].reg;
						++insn;

						// LDUR is used when offset is negative
						INSN_ASSERT(insn.id() == ARM64_INS_LDR || insn.id() == ARM64_INS_LDUR);
						//INSN_ASSERT(insn.ops[0].reg == tmpReg); // ops[0] might be decimal
						INSN_ASSERT(insn.ops[1].mem.base == tmpReg && insn.ops[1].mem.disp == 8 * (1 - i));
						const auto valReg = insn.ops[0].reg;
						fnInfo->params.params.push_back(FnParamInfo{ A64::Register{ valReg } });
						++insn;

						// the parameter value might be saved to stack as if it is a local variable but with fixed negative offset from FP
						if (insn.id() == ARM64_INS_STUR) {
							INSN_ASSERT(insn.ops[0].reg == valReg);
							INSN_ASSERT(insn.ops[1].mem.base == CSREG_DART_FP);
							fnInfo->params.params.back().localOffset = insn.ops[1].mem.disp;
							++insn;
						}
					}
					else {
						fnInfo->params.params.push_back(FnParamInfo{ A64::Register{} });
					}

					++i;
				}

				if (!missingBranchTargets.empty()) {
					// all params are passed
					while (true) {
						// might unbox parameters (int, double) to native value
						const auto& [dstReg, srcReg] = unboxParam();
						if (!dstReg.IsSet())
							break;

						auto param = fnInfo->params.findValReg(srcReg);
						INSN_ASSERT(param != nullptr);
						param->type = app.TypeDb()->Get(dstReg.IsDecimal() ? dart::kDoubleCid : app.DartIntCid());
						param->valReg = dstReg;
					}

					// might be move to another register
					while (!insn.IsBranch()) {
						INSN_ASSERT(insn.id() == ARM64_INS_MOV);
						INSN_ASSERT(insn.ops[0].type == ARM64_OP_REG && insn.ops[1].type == ARM64_OP_REG);
						const auto srcReg = A64::Register{ insn.ops[1].reg };
						auto found = fnInfo->params.movValReg(A64::Register{ insn.ops[0].reg }, srcReg);
						INSN_ASSERT(found);
						++insn;
					}
					const auto storingBranchTarget = insn.ops[0].imm;
					++insn;

					// skip branches that only for missing some parameters to missing all parameters
					const auto num_optional_param = missingBranchTargets.size();
					while (insn.address() != missingBranchTargets.front()) {
						while (!insn.IsBranch())
							++insn;
						++insn;
					}

					// default values (missing all parameters branch)
					std::vector<FnParamInfo> optParams2;
					while (insn.address() < storingBranchTarget) {
						// might be load from PP, mov from constant, mov for moving register
						auto res = processLoadValueInstr(insn);
						if (res.lastIns) {
							auto* il = res.get<LoadValueInstr>();
							optParams2.push_back(FnParamInfo{ il->dstReg, il->val.Value() });
							insn = res.lastIns + 1;
						}
						else if (insn.id() == ARM64_INS_MOV) {
							// moving register
							INSN_ASSERT(insn.ops[0].type == ARM64_OP_REG && insn.ops[1].type == ARM64_OP_REG);
							const auto srcReg = A64::Register{ insn.ops[1].reg };
							auto itr = std::find_if(optParams2.begin(), optParams2.end(), [&](auto const& param) { return param.valReg == srcReg; });
							INSN_ASSERT(itr != optParams2.end());
							itr->valReg = A64::Register{ insn.ops[0].reg };
							++insn;
						}
						else {
							FATAL("unexpected instruction");
						}
					}

					INSN_ASSERT(optParams2.size() <= fnInfo->params.params.size() - fnInfo->params.numFixedParam);
					// Note: optParams2 MUST be ordered same as previous steps but some named parameter might have no default value (required or unused)
					for (int i = fnInfo->params.numFixedParam, j = 0; i < fnInfo->params.params.size(); i++) {
						if (fnInfo->params.params[i].valReg.IsSet()) {
							ASSERT(fnInfo->params.params[i].valReg == optParams2[j].valReg);
							INSN_ASSERT(j < optParams2.size());
							fnInfo->params.params[i].val = optParams2[j].val;
							j++;
						}
					}

					while (insn.id() == ARM64_INS_STUR) {
						INSN_ASSERT(insn.ops[1].mem.base == CSREG_DART_FP);
						const auto srcReg = A64::Register{ insn.ops[0].reg };
						auto param = fnInfo->params.findValReg(srcReg);
						INSN_ASSERT(param != nullptr);
						param->localOffset = insn.ops[1].mem.disp;
						++insn;
					}
				}

				// both optional positional parameter and named parameter cannot be used in a function declaration
				// so no named parameter
				--insn;
				return ILResult{ insn.ptr(), std::make_unique<SetupParametersInstr>(AddrRange(insn0_addr, insn.NextAddress()), &fnInfo->params) };
			}

			// check for named parameters case
			// load the first named parameter name from ArgumentDescriptor (generated code uses constant for access first named parameter)
			if (insn.id() == ARM64_INS_LDUR && insn.ops[1].mem.base == argsDescReg && insn.ops[1].mem.disp == AOT_ArgumentsDescriptor_first_named_entry_offset - dart::kHeapObjectTag)
			{
				// names of the optional parameters are alphabetically sorted
				// so the code can find all named parameters in one iteration
				int nameParamCnt = 0;
				auto paramIdxReg = ARM64_REG_INVALID;
				A64::Register currParamPosReg;
				A64::Register currParamPosSmiReg;
				auto currParamOffsetReg = ARM64_REG_INVALID;
				bool isLastName = false;
				bool isRequired = false;

				// ## loop of checking name and loading value
				// curr_param_pos = 0
				// if argDesc[curr_param_pos].name == "name1"
				//    load argument position in stack from argDesc[curr_param_pos].pos
				//    load param value from stack (call arguments)
				//    increment curr_param_pos
				// else
				//    load default value
				// might store value into local variable
				// ... if argDesc[curr_param_pos].name == "name2"
				// ...
				// ## for "required" argument, there is no name check. psuedo code will be
				// load argument position in stack from argDesc[curr_param_pos].pos
				// load param value from stack (call arguments)
				// increment curr_param_pos
				// ## the first name is handled specifically because curr_param_pos is known value
				// ## the last name is a bit different too because no need for incrementing curr_param_pos
				while (!isLastName) {
					// load current name from ArgumentsDescriptor
					if (nameParamCnt) {
						// load from currParamPosReg
						// 0x4128dc: lsl  x5, x2, #1
						// 0x4128e0: lsl  w6, w5, #1
						// 0x4128e4: add  w7, w6, #8
						// 0x4128e8: add  x16, x0, w7, sxtw #1
						// 0x4128ec: ldur  w8, [x16, #0xf]
						if (!isRequired) {
							// make it be Smi
							INSN_ASSERT(insn.id() == ARM64_INS_LSL);
							INSN_ASSERT(A64::Register{ insn.ops[1].reg } == currParamPosReg);
							INSN_ASSERT(insn.ops[2].imm == 1);
							currParamPosSmiReg = insn.ops[0].reg;
							++insn;
						}

						// weird case. add 1 to currParamPosSmiReg. maybe this named parameter is not used and skip it.
						if (insn.id() == ARM64_INS_ADD) {
							INSN_ASSERT(A64::Register{ insn.ops[1].reg } == currParamPosSmiReg);
							INSN_ASSERT(insn.ops[2].imm == 2); // tagged value of 1
							currParamPosSmiReg = insn.ops[0].reg;
							++insn;
						}

						// skip to next named parameter (multiply by 2 because name and index pair)
						INSN_ASSERT(insn.id() == ARM64_INS_LSL);
						INSN_ASSERT(A64::Register{ insn.ops[1].reg } == currParamPosSmiReg);
						INSN_ASSERT(insn.ops[2].imm == 1);
						currParamOffsetReg = insn.ops[0].reg;
						++insn;

						INSN_ASSERT(insn.id() == ARM64_INS_ADD);
						INSN_ASSERT(insn.ops[1].reg == currParamOffsetReg);
						if (insn.ops[2].imm == 8) {
							const auto tmpReg = insn.ops[0].reg;
							++insn;

							INSN_ASSERT(insn.id() == ARM64_INS_ADD);
							INSN_ASSERT(insn.ops[1].reg == argsDescReg);
							INSN_ASSERT(insn.ops[2].reg == tmpReg && insn.ops[2].ext == ARM64_EXT_SXTW && insn.ops[2].shift.value == 1);
							const auto tmpReg2 = insn.ops[0].reg;
							++insn;

							INSN_ASSERT(insn.id() == ARM64_INS_LDUR);
							INSN_ASSERT(insn.ops[1].mem.base == tmpReg2 && insn.ops[1].mem.disp == sizeof(void*) * 2 - dart::kHeapObjectTag);
							paramIdxReg = ToCapstoneReg(insn.ops[0].reg);
							++insn;
							isRequired = false;
						}
						else {
							INSN_ASSERT(insn.ops[2].imm == 0xa);
							isRequired = true;
						}
					}
					else {
						// first name is loaded from fixed offset
						paramIdxReg = ToCapstoneReg(insn.ops[0].reg);
						++insn;
					}

					std::string paramName;
					if (!isRequired) {
						// decompress pointer
						INSN_ASSERT(insn.id() == ARM64_INS_ADD);
						INSN_ASSERT(insn.ops[0].reg == insn.ops[1].reg && insn.ops[0].reg == paramIdxReg);
						INSN_ASSERT(insn.ops[2].reg == CSREG_DART_HEAP && insn.ops[2].shift.value == 32);
						++insn;

						// get expected parameter name from PP
						const auto objPoolInstr = processGetObjectPoolInstruction(app, insn);
						INSN_ASSERT(objPoolInstr.dstReg == A64::TMP_REG);
						INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kStringCid);
						paramName = objPoolInstr.item.Get<VarString>()->str;
						insn += objPoolInstr.insCnt;

						INSN_ASSERT(insn.id() == ARM64_INS_CMP);
						INSN_ASSERT(ToCapstoneReg(insn.ops[0].reg) == paramIdxReg && A64::Register{ insn.ops[1].reg } == objPoolInstr.dstReg);
						++insn;

						if (insn.IsBranch(ARM64_CC_EQ)) {
							// last named parameter and not used
							const auto branchTarget = insn.ops[0].imm;
							++insn;
							INSN_ASSERT(branchTarget == insn.address());
							fnInfo->params.params.push_back(FnParamInfo{ std::move(paramName) });
							++nameParamCnt;
							break;
						}

						INSN_ASSERT(insn.IsBranch(ARM64_CC_NE));
						//const auto wrongNameAddr = insn.ops[0].imm;
						++insn;
					}
					else {
						paramName = "required";
					}

					// parameter name matches. load the value from stack.
					// Note: some parameter is not loaded at all
					auto argIdxReg = ARM64_REG_INVALID;
					if (nameParamCnt) {
						// 0x412904: add  w2, w6, #0xa
						// 0x412908: add  x16, x0, w2, sxtw #1
						// 0x41290c: ldur  w6, [x16, #0xf]
						if (insn.id() == ARM64_INS_ADD && insn.ops[1].reg == currParamOffsetReg) {
							INSN_ASSERT(insn.ops[2].imm == 0xa);
							const auto tmpReg = insn.ops[0].reg;
							++insn;

							INSN_ASSERT(insn.id() == ARM64_INS_ADD);
							INSN_ASSERT(insn.ops[1].reg == argsDescReg);
							INSN_ASSERT(insn.ops[2].reg == tmpReg && insn.ops[2].ext == ARM64_EXT_SXTW && insn.ops[2].shift.value == 1);
							const auto tmpReg2 = insn.ops[0].reg;
							++insn;

							INSN_ASSERT(insn.id() == ARM64_INS_LDUR);
							INSN_ASSERT(insn.ops[1].mem.base == tmpReg2 && insn.ops[1].mem.disp == sizeof(void*) * 2 - dart::kHeapObjectTag);
							argIdxReg = ToCapstoneReg(insn.ops[0].reg);
							++insn;
						}
					}
					else {
						if (insn.id() == ARM64_INS_LDUR) {
							INSN_ASSERT(insn.ops[1].mem.base == argsDescReg && insn.ops[1].mem.disp == AOT_ArgumentsDescriptor_first_named_entry_offset + AOT_ArgumentsDescriptor_position_offset - dart::kHeapObjectTag);
							argIdxReg = ToCapstoneReg(insn.ops[0].reg);
							++insn;
						}
					}

					if (argIdxReg != ARM64_REG_INVALID) {
						// function arguments index in stack

						INSN_ASSERT(insn.id() == ARM64_INS_ADD); // decompress pointer
						INSN_ASSERT(insn.ops[0].reg == insn.ops[1].reg && insn.ops[0].reg == argIdxReg);
						INSN_ASSERT(insn.ops[2].reg == CSREG_DART_HEAP && insn.ops[2].shift.value == 32);
						++insn;

						// fetch name parameter value
						// 0x4128bc: sub  w4, w1, w2  ; total_param - args_idx
						// 0x4128c0: add  x2, x29, w4, sxtw #2
						// 0x4128c4: ldr  x2, [x2, #8]
						INSN_ASSERT(insn.id() == ARM64_INS_SUB);
						INSN_ASSERT(ToCapstoneReg(insn.ops[1].reg) == paramCntReg);
						INSN_ASSERT(ToCapstoneReg(insn.ops[2].reg) == argIdxReg);
						const auto tmpReg = insn.ops[0].reg;
						++insn;

						INSN_ASSERT(insn.id() == ARM64_INS_ADD);
						INSN_ASSERT(insn.ops[1].reg == CSREG_DART_FP);
						INSN_ASSERT(insn.ops[2].reg == tmpReg && insn.ops[2].ext == ARM64_EXT_SXTW && insn.ops[2].shift.value == 2);
						const auto tmpReg2 = insn.ops[0].reg;
						++insn;

						INSN_ASSERT(insn.id() == ARM64_INS_LDR);
						INSN_ASSERT(insn.ops[0].reg == tmpReg2);
						INSN_ASSERT(insn.ops[1].mem.base == tmpReg2 && insn.ops[1].mem.disp == sizeof(void*));
						auto valReg = A64::Register{ insn.ops[0].reg };
						++insn;

						fnInfo->params.params.push_back(FnParamInfo{ valReg, std::move(paramName) });
					}
					else {
						fnInfo->params.params.push_back(FnParamInfo{ std::move(paramName) });
					}

					// for the first parameter name, this will be set with MOVZ (after unboxing)
					if (nameParamCnt) {
						// if this is not last named parameter, increment the param position (smi)
						if (insn.id() == ARM64_INS_ADD) {
							// 0x412920: add  w2, w5, #2
							INSN_ASSERT(A64::Register{ insn.ops[1].reg } == currParamPosSmiReg);
							INSN_ASSERT(insn.ops[2].imm == 2);
							currParamPosSmiReg = insn.ops[0].reg;
							++insn;
						}
						else {
							isLastName = true;
						}
					}

					if (argIdxReg != ARM64_REG_INVALID) {
						// unbox param for int and double
						const auto& [dstReg, srcReg] = unboxParam(fnInfo->params.params.back().valReg);
						if (dstReg.IsSet()) {
							INSN_ASSERT(srcReg == fnInfo->params.params.back().valReg);
							fnInfo->params.params.back().type = app.TypeDb()->Get(dstReg.IsDecimal() ? dart::kDoubleCid : app.DartIntCid());
							fnInfo->params.params.back().valReg = dstReg;
						}
					}

					if (!isRequired) {
						// Smi to native. only non first and last name do it
						if (nameParamCnt && !isLastName) {
							// 0x412924: sbfx  x5, x2, #1, #0x1f
							if (insn.id() == ARM64_INS_SBFX) {
								INSN_ASSERT(A64::Register{ insn.ops[1].reg } == currParamPosSmiReg);
								INSN_ASSERT(insn.ops[2].imm == 1 && insn.ops[3].imm == 0x1f);
								currParamPosReg = insn.ops[0].reg;
								++insn;
							}
						}
					}

					// doing loop here because the moving value to another register can be in any order and multiple time
					while (insn.id() == ARM64_INS_MOV && insn.ops[1].type == ARM64_OP_REG && insn.ops[1].reg != CSREG_DART_NULL) {
						const auto srcReg = A64::Register{ insn.ops[1].reg };

						if (!isRequired && nameParamCnt && !isLastName) {
							// change param pos register
							if (srcReg == currParamPosReg) {
								currParamPosReg = insn.ops[0].reg;
								++insn;
								continue;
							}
						}

						if (argIdxReg != ARM64_REG_INVALID) {
							// move register value to another register
							if (srcReg == fnInfo->params.params.back().valReg) {
								fnInfo->params.params.back().valReg = A64::Register{ insn.ops[0].reg };
								++insn;
								continue;
							}
						}

						// not register for this param. check for all previous parameters.
						bool found = fnInfo->params.movValReg(A64::Register{ insn.ops[0].reg }, srcReg);
						INSN_ASSERT(found);
						++insn;
					}

					// first param name, set the param pos value
					if (!nameParamCnt) {
						if (insn.IsMovz() && insn.ops[1].imm == 1) {
							currParamPosReg = insn.ops[0].reg;
							++insn;
						}
						else {
							isLastName = true;
						}
					}

					if (!isRequired) {
						int64_t nextParamAddr = 0;
						if (argIdxReg != ARM64_REG_INVALID || nameParamCnt == 0) {
							// branch for skipping loading default value
							INSN_ASSERT(insn.IsBranch());
							nextParamAddr = insn.ops[0].imm;
							++insn;
						}

						while (insn.id() == ARM64_INS_MOV && insn.ops[1].type == ARM64_OP_REG && insn.ops[1].reg != CSREG_DART_NULL) {
							// param pos register is changed
							if (nameParamCnt && !isLastName && insn.id() == ARM64_INS_MOV && A64::Register{ insn.ops[0].reg } == currParamPosReg) {
								++insn;
								continue;
							}
							// other moving reg is this branch should be same as loading param value branch
							++insn;
						}

						const auto processAssignParamPos0 = [&] {
							if (insn.IsMovz() && A64::Register{ insn.ops[0].reg } == currParamPosReg && insn.ops[1].imm == 0) {
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
						if (argIdxReg != ARM64_REG_INVALID) {
							// load default value
							auto res = processLoadValueInstr(insn);
							if (!res.lastIns)
								break;
							auto* il = res.get<LoadValueInstr>();
							INSN_ASSERT(il->dstReg == fnInfo->params.params.back().valReg);
							fnInfo->params.params.back().val = il->val.Value();
							insn = res.lastIns + 1;
						}

						if (!nameParamCnt && !isLastName && !foundAssignParamPos0) {
							foundAssignParamPos0 = processAssignParamPos0();
						}

						INSN_ASSERT(nextParamAddr == 0 || insn.address() == nextParamAddr);
					}

					if (argIdxReg != ARM64_REG_INVALID) {
						// this value might be stored into a local variable
						if (insn.id() == ARM64_INS_STUR && A64::Register{ insn.ops[0].reg } == fnInfo->params.params.back().valReg && insn.ops[1].mem.base == CSREG_DART_FP) {
							fnInfo->params.params.back().localOffset = insn.ops[1].mem.disp;
							++insn;
						}
					}
					// end of a named parameter
					++nameParamCnt;

					// extra check for end of named parameters
					if (insn.id() != ARM64_INS_LSL) {
						break;
					}
				}

				fnInfo->params.isNamedParam = true;
				--insn;
				return ILResult{ insn.ptr(), std::make_unique<SetupParametersInstr>(AddrRange(insn0_addr, insn.NextAddress()), &fnInfo->params) };
			}
		}
		
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processSaveRegisterInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_STR && insn.ops[1].mem.base == CSREG_DART_SP && insn.writeback()) {
		INSN_ASSERT(insn.ops[1].mem.disp == -GetCsRegSize(insn.ops[0].reg));
		return ILResult{ insn.ptr(), std::make_unique<SaveRegisterInstr>(insn.ptr(), A64::Register{ insn.ops[0].reg }) };
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processLoadSavedRegisterInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_LDR && insn.ops[1].mem.base == CSREG_DART_SP && insn.writeback()) {
		INSN_ASSERT(insn.ops[2].imm == GetCsRegSize(insn.ops[0].reg));
		return ILResult{ insn.ptr(), std::make_unique<RestoreRegisterInstr>(insn.ptr(), A64::Register{ insn.ops[0].reg }) };
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processCallInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_BL) {
		if (insn.ops[0].type == ARM64_OP_IMM) {
			const auto target = insn.ops[0].imm;
			return ILResult{ insn.ptr(), std::make_unique<CallInstr>(insn.ptr(), app.GetFunction(target), (uint64_t)target) };
		}
		// should indirect call handle here
	}
	else if (insn.id() == ARM64_INS_B && insn.ops[0].type == ARM64_OP_IMM) {
		// this might tail branch (function call at the end of function)
		const auto target = insn.ops[0].imm;
		if (target < dartFn->Address() || target >= dartFn->AddressEnd()) {
			return ILResult{ insn.ptr(), std::make_unique<CallInstr>(insn.ptr(), app.GetFunction(target), (uint64_t)target) };
		}
	}

	return ILResult{};
}

ILResult FunctionAnalyzer::processLoadFieldTableInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_LDR && insn.ops[1].mem.base == CSREG_DART_THR && insn.ops[1].mem.disp == dart::Thread::field_table_values_offset()) {
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
		const auto insn0_addr = insn.address();

		const auto result_reg = insn.ops[0].reg;
		const auto dstReg = A64::Register{ result_reg };
		auto tmp_reg = insn.ops[0].reg;
		auto load_offset = 0;

		++insn;
		if (insn.id() == ARM64_INS_ADD && insn.ops[1].reg == tmp_reg && insn.ops[2].type == ARM64_OP_IMM && insn.ops[2].shift.type == ARM64_SFT_LSL) {
			// large static field offset
			tmp_reg = insn.ops[0].reg;
			load_offset = insn.ops[2].imm << insn.ops[2].shift.value;
			++insn;
		}

		if (insn.id() != ARM64_INS_STR && insn.id() != ARM64_INS_LDR) {
			FATAL("static field without STR or LDR");
		}

		INSN_ASSERT(insn.ops[1].mem.base == tmp_reg);
		load_offset |= insn.ops[1].mem.disp;
		const auto field_offset = load_offset >> 1;

		if (insn.id() == ARM64_INS_STR) {
			return ILResult{ insn.ptr(), std::make_unique<StoreStaticFieldInstr>(AddrRange(insn0_addr, insn.NextAddress()), A64::Register{ insn.ops[0].reg }, field_offset) };
		}
		else {
			INSN_ASSERT(insn.ops[0].reg == result_reg);

			auto loadStaticInstr = std::make_unique<LoadStaticFieldInstr>(AddrRange(insn0_addr, insn.NextAddress()), dstReg, field_offset);
			auto loadStatic_last_ptr = insn.ptr();

			++insn;
			// load Sentinel from PP to check if this field is initialized?
			const auto objPoolInstr = processGetObjectPoolInstruction(app, insn);
			if (objPoolInstr.insCnt == 0 || objPoolInstr.dstReg != A64::TMP_REG || objPoolInstr.item.ValueTypeId() != dart::kSentinelCid) {
				// this is just load static field (same as global variable). cannot find the owner
				return ILResult{ loadStatic_last_ptr, std::move(loadStaticInstr) };
			}

			// if any condition is not met, fallback to just load static field
			insn += objPoolInstr.insCnt;
			INSN_ASSERT(insn.id() == ARM64_INS_CMP);
			INSN_ASSERT(A64::Register{ insn.ops[0].reg } == dstReg);
			INSN_ASSERT(A64::Register{ insn.ops[1].reg } == A64::TMP_REG);

			++insn;
			INSN_ASSERT(insn.id() == ARM64_INS_B);
			if (insn.cc() == ARM64_CC_NE) {
				const auto cont_addr = insn.ops[0].imm;

				// get pool object
				++insn;
				const auto objPoolInstr = processGetObjectPoolInstruction(app, insn);
				if (objPoolInstr.insCnt > 0) {
					INSN_ASSERT(objPoolInstr.dstReg == A64::Register{ dart::InitStaticFieldABI::kFieldReg });
					INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kFieldCid);
					auto& dartField = objPoolInstr.item.Get<VarField>()->field;
					INSN_ASSERT(dartField.Offset() == field_offset);
					insn += objPoolInstr.insCnt;

					// GenerateStubCall()
					// - EmitCallToStub() might be GenerateUnRelocatedPcRelativeCall() or BranchLink()
					if (insn.id() == ARM64_INS_BL) {
						auto dartFn = app.GetFunction(insn.ops[0].imm);
						ASSERT(dartFn->IsStub());
						const auto stubKind = reinterpret_cast<DartStub*>(dartFn)->kind;
						INSN_ASSERT(stubKind == DartStub::InitLateStaticFieldStub || stubKind == DartStub::InitLateFinalStaticFieldStub);
					}
					else {
						// BranchLink()
						const auto objPoolInstr = processGetObjectPoolInstruction(app, insn);
						INSN_ASSERT(objPoolInstr.dstReg == A64::Register{ dart::CODE_REG });
						// TODO: what kind of object is loaded?
						insn += objPoolInstr.insCnt;

						INSN_ASSERT(insn.id() == ARM64_INS_LDUR);
						INSN_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
						INSN_ASSERT(insn.ops[1].mem.base == ToCapstoneReg(dart::CODE_REG));
						INSN_ASSERT(insn.ops[1].mem.disp == dart::Code::entry_point_offset(dart::CodeEntryKind::kNormal) - dart::kHeapObjectTag);
						++insn;

						INSN_ASSERT(insn.id() == ARM64_INS_BLR);
						INSN_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
					}


					INSN_ASSERT(insn.NextAddress() == cont_addr);

					return ILResult{ insn.ptr(), std::make_unique<InitLateStaticFieldInstr>(AddrRange(insn0_addr, insn.NextAddress()), dstReg, dartField) };
				}
			}
			else {
				// late intialize error
				INSN_ASSERT(insn.cc() == ARM64_CC_EQ);
				const auto error_addr = insn.ops[0].imm;
				INSN_ASSERT(error_addr > insn.NextAddress() && error_addr < dartFn->AddressEnd());
				
				auto insn2 = insn.MoveTo(error_addr);
				const auto objPoolInstr = processGetObjectPoolInstruction(app, insn2);
				if (objPoolInstr.insCnt > 0 && objPoolInstr.dstReg == A64::Register{ dart::LateInitializationErrorABI::kFieldReg } && objPoolInstr.item.ValueTypeId() == dart::kFieldCid) {
					auto& dartField = objPoolInstr.item.Get<VarField>()->field;
					INSN_ASSERT(dartField.Offset() == field_offset);

					insn2 += objPoolInstr.insCnt;
					INSN_ASSERT(insn2.id() == ARM64_INS_BL && insn2.ops[0].type == ARM64_OP_IMM);
					auto fn = app.GetFunction(insn2.ops[0].imm);
					auto stub = fn->AsStub();
					INSN_ASSERT(stub->kind == DartStub::LateInitializationErrorSharedWithoutFPURegsStub || stub->kind == DartStub::LateInitializationErrorSharedWithFPURegsStub);
					// load static field but throw an exception if it is not initialized
					return ILResult{ insn.ptr(), std::make_unique<LoadStaticFieldInstr>(AddrRange(insn0_addr, insn.NextAddress()), dstReg, field_offset) };
				}
			}

			// this one might be for if/else when Static field is not initialized
			return ILResult{ loadStatic_last_ptr, std::move(loadStaticInstr) };
		}
	}

	return ILResult{};
}

ILResult FunctionAnalyzer::processLoadImmInstr(AsmInstruction insn)
{
	auto insn0 = insn.ptr();
	int64_t imm = 0;
	arm64_reg dstReg = ARM64_REG_INVALID;
	// libcapstone5 use MOV instead of MOVZ
	if (insn.IsMovz()) {
		imm = insn.ops[1].imm;
		dstReg = insn.ops[0].reg;

		++insn;
		if (insn.id() == ARM64_INS_MOVK && insn.ops[0].reg == dstReg && insn.ops[1].shift.value == 16) {
			imm |= insn.ops[1].imm << 16;
		}
		else {
			--insn;
		}
	}
	//else if (insn.id() == ARM64_INS_MOV && insn.ops[1].reg == CSREG_DART_NULL) {
	//}
	else if (insn.id() == ARM64_INS_ORR && insn.ops[1].reg == ARM64_REG_XZR && insn.ops[2].type == ARM64_OP_IMM) {
		imm = insn.ops[2].imm;
		dstReg = insn.ops[0].reg;
	}
	else if (insn.id() == ARM64_INS_MOVN) {
		imm = insn.ops[1].imm << insn.ops[1].shift.value;
		imm = ~imm;
		dstReg = insn.ops[0].reg;
	}

	if (dstReg != ARM64_REG_INVALID) {
		return ILResult{ insn.ptr(), std::make_unique<LoadImmInstr>(AddrRange(insn0->address, insn.NextAddress()), A64::Register{ dstReg }, imm) };
	}

	return ILResult{};
}

ILResult FunctionAnalyzer::processGdtCallInstr(AsmInstruction insn)
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
	if (insn.ops[0].reg == CSREG_DART_LR && (insn.id() == ARM64_INS_ADD || insn.id() == ARM64_INS_SUB) && 
		insn.ops[1].reg == ToCapstoneReg(dart::DispatchTableNullErrorABI::kClassIdReg))
	{
		// this is GDT call
		auto insn0_addr = insn.ptr()->address;
		int64_t offset = 0;

		if (insn.ops[2].type == ARM64_OP_IMM) {
			//cidReg = insn.ops[1].reg;
			offset = insn.ops[2].imm;
			if (insn.ops[2].shift.type != ARM64_SFT_INVALID) {
				ASSERT(insn.ops[2].shift.type == ARM64_SFT_LSL);
				offset <<= insn.ops[2].shift.value;
			}
			if (insn.id() == ARM64_INS_SUB)
				offset = -offset;
		}
		else {
			// from reg
			INSN_ASSERT(insn.id() == ARM64_INS_ADD);
			INSN_ASSERT(insn.ops[2].type == ARM64_OP_REG && insn.ops[2].reg == CSREG_DART_TMP2);

			auto il_loadImm = reinterpret_cast<LoadImmInstr*>(fnInfo->LastIL());
			INSN_ASSERT(il_loadImm->dstReg == A64::TMP2_REG);
			offset = il_loadImm->val;
			insn0_addr = il_loadImm->Start();
			fnInfo->RemoveLastIL();
		}
		//const auto selector_offset = dart::DispatchTable::kOriginElement + offset;
		++insn;
		INSN_ASSERT(insn.id() == ARM64_INS_LDR);
		INSN_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
		ASSERT(insn.ops[1].mem.base == CSREG_DART_DISPATCH_TABLE && insn.ops[1].mem.index == CSREG_DART_LR && insn.ops[1].shift.value == 3);

		++insn;
		INSN_ASSERT(insn.id() == ARM64_INS_BLR);
		INSN_ASSERT(insn.ops[0].reg == CSREG_DART_LR);

		return ILResult{ insn.ptr(), std::make_unique<GdtCallInstr>(AddrRange(insn0_addr, insn.NextAddress()), offset) };
	}

	return ILResult{};
}

ILResult FunctionAnalyzer::processReturnInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_RET) {
		return ILResult{ insn.ptr(), std::make_unique<ReturnInstr>(insn.ptr()) };
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processInstanceofNoTypeArgumentInstr(AsmInstruction insn)
{
	// cannot find where the compiler generate setting kInstanceReg, kInstantiatorTypeArgumentsReg and kFunctionTypeArgumentsReg code
	// FlowGraphCompiler::GenerateInstanceOf()
	// - FlowGraphCompiler::GenerateInlineInstanceof()
	//   - FlowGraphCompiler::GenerateInstantiatedTypeNoArgumentsTest()
	const auto insn0_addr = insn.address();
	const auto srcReg = [&] {
		if (insn.id() == ARM64_INS_MOV && insn.ops[0].reg == ToCapstoneReg(dart::TypeTestABI::kInstanceReg)) {
			const auto srcReg = insn.ops[1].reg;
			++insn;
			if (insn.id() == ARM64_INS_MOV && insn.ops[0].reg == ToCapstoneReg(dart::TypeTestABI::kInstantiatorTypeArgumentsReg) && insn.ops[1].reg == CSREG_DART_NULL) {
				++insn;
				if (insn.id() == ARM64_INS_MOV && insn.ops[0].reg == ToCapstoneReg(dart::TypeTestABI::kFunctionTypeArgumentsReg) && insn.ops[1].reg == CSREG_DART_NULL) {
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
		auto il_res1 = processBranchIfSmiInstr(insn);
		// TODO: there might be check for NULL here
		if (!il_res1.lastIns)
			return ILResult{};
		INSN_ASSERT(il_res1.lastIns);
		insn = il_res1.lastIns + 1;
		const auto ilBranch = il_res1.get<BranchIfSmiInstr>();
		INSN_ASSERT(ilBranch->objReg == A64::Register{ dart::TypeTestABI::kInstanceReg });
		const auto done_addr = ilBranch->branchAddr;

		// int and num type are extracted for quick check. other object just use stub for checking.
		intptr_t typeCheckCid = 0;
		auto il_res2 = processLoadClassIdInstr(insn);
		if (il_res2.lastIns) {
			insn = il_res2.lastIns + 1;
			const auto ilLoadCid = il_res2.get<LoadClassIdInstr>();
			INSN_ASSERT(ilLoadCid->objReg == A64::Register{ dart::TypeTestABI::kInstanceReg });
			INSN_ASSERT(ilLoadCid->cidReg == A64::Register{ dart::TypeTestABI::kScratchReg });

			INSN_ASSERT(insn.id() == ARM64_INS_SUB);
			INSN_ASSERT(insn.ops[0].reg == ToCapstoneReg(dart::TypeTestABI::kScratchReg));
			INSN_ASSERT(insn.ops[1].reg == ToCapstoneReg(dart::TypeTestABI::kScratchReg));
			INSN_ASSERT(insn.ops[2].imm == dart::kSmiCid); // kMintCid is always be kSmiCid+1
			++insn;

			INSN_ASSERT(insn.id() == ARM64_INS_CMP);
			INSN_ASSERT(insn.ops[0].reg == ToCapstoneReg(dart::TypeTestABI::kScratchReg));
			INSN_ASSERT(insn.ops[1].imm == 1 || insn.ops[1].imm == 2); // must be kSmiCid or kMintCid or kDoubleCid
			typeCheckCid = insn.ops[1].imm == 1 ? app.DartIntCid() : dart::kNumberCid;
			++insn;

			INSN_ASSERT(insn.IsBranch(ARM64_CC_LS));
			INSN_ASSERT(insn.ops[0].imm == done_addr);
			++insn;
		}

		// kDstTypeReg from PP
		const auto objPoolInstr = processGetObjectPoolInstruction(app, insn);
		INSN_ASSERT(objPoolInstr.dstReg == A64::Register{ dart::TypeTestABI::kDstTypeReg });
		INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kTypeCid);
		const auto vtype = objPoolInstr.item.Get<VarType>();
		INSN_ASSERT(typeCheckCid == 0 || typeCheckCid == vtype->type.Class().Id());
		insn += objPoolInstr.insCnt;

		// kSubtypeTestCacheReg from PP
		const auto objPoolInstr2 = processGetObjectPoolInstruction(app, insn);
		INSN_ASSERT(objPoolInstr2.dstReg == A64::Register{ dart::TypeTestABI::kSubtypeTestCacheReg });
		INSN_ASSERT(objPoolInstr2.item.Value()->RawTypeId() == dart::kNullCid);
		insn += objPoolInstr2.insCnt;

		INSN_ASSERT(insn.id() == ARM64_INS_BL);
		INSN_ASSERT(insn.NextAddress() == done_addr);
		auto dartFn = app.GetFunction(insn.ops[0].imm);
		auto dartStub = dartFn->AsStub();
		auto typeName = dartStub->Name();
		if (typeCheckCid == app.DartIntCid()) {
			INSN_ASSERT(dartStub->kind == DartStub::TypeCheckStub);
			INSN_ASSERT(typeName == "int" || typeName == "int?");
		}
		else { // typeCheckCid == dart::kNumberCid || typeCheckCid == 0 (object)
			INSN_ASSERT(typeName == vtype->ToString() || dartStub->kind == DartStub::DefaultTypeTestStub);
		}
		return ILResult{ insn.ptr(), std::make_unique<TestTypeInstr>(AddrRange(insn0_addr, insn.NextAddress()), A64::Register{ srcReg }, objPoolInstr.item.Get<VarType>()->ToString()) };
	}

	return ILResult{};
}

ILResult FunctionAnalyzer::processBranchIfSmiInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_TBZ && insn.ops[1].imm == dart::kSmiTag && dart::kCompressedWordSize == GetCsRegSize(insn.ops[0].reg)) {
		const auto objReg = A64::Register{ insn.ops[0].reg };
		const auto branchAddr = insn.ops[2].imm;
		return ILResult{ insn.ptr(), std::make_unique<BranchIfSmiInstr>(insn.ptr(), objReg, branchAddr) };
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processLoadClassIdInstr(AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_LDUR && insn.ops[1].mem.disp == -1 && dart::UntaggedObject::kClassIdTagPos == 12) {
		auto insn0 = insn.ptr();
		// 0x21cb10: ldur  x1, [x0, #-1]  ; load object tag
		const auto objReg = A64::Register{ insn.ops[1].mem.base };
		const auto cidReg = insn.ops[0].reg;

		++insn;
		// Assembler::ExtractClassIdFromTags()
		// 0x21cb14: ubfx  x1, x1, #0xc, #0x14  ; extract object class id
		INSN_ASSERT(insn.id() == ARM64_INS_UBFX);
		INSN_ASSERT(insn.ops[0].reg == cidReg);
		INSN_ASSERT(insn.ops[1].reg == cidReg);
		INSN_ASSERT(insn.ops[2].imm == dart::UntaggedObject::kClassIdTagPos);
		INSN_ASSERT(insn.ops[3].imm == dart::UntaggedObject::kClassIdTagSize);
		return ILResult{ insn.ptr(), std::make_unique<LoadClassIdInstr>(AddrRange(insn0->address, insn.NextAddress()), objReg, A64::Register{cidReg}) };
	}
	else if (insn.id() == ARM64_INS_LDURH && insn.ops[1].mem.disp == 1 && dart::UntaggedObject::kClassIdTagPos == 16) {
		// https://github.com/dart-lang/sdk/commit/9182d5e5359988703a2b8a88c238f47a5295e18c
		// 0x5f3900: ldurh  w3, [x0, #1]
		const auto objReg = A64::Register{ insn.ops[1].mem.base };
		const auto cidReg = insn.ops[0].reg;
		return ILResult{ insn.ptr(), std::make_unique<LoadClassIdInstr>(AddrRange(insn.address(), insn.NextAddress()), objReg, A64::Register{cidReg}) };
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processLoadTaggedClassIdMayBeSmiInstr(AsmInstruction insn)
{
	//   cidReg = SmiTaggedClassId
	//   branchIfSmi(objReg, branchAddr)
	//   cidReg = LoadClassIdInstr(objReg)
	//   lsl  cidReg, cidReg, #1
	// branchAddr:
	auto& il = fnInfo->il_insns.back();
	if (insn.id() == ARM64_INS_LSL && insn.ops[0].reg == insn.ops[1].reg && insn.ops[2].imm == dart::kSmiTagSize && il->Kind() == ILInstr::LoadClassId && fnInfo->il_insns.size() >= 3) {
		auto il_loadClassId = reinterpret_cast<LoadClassIdInstr*>(il.get());
		if (il_loadClassId->cidReg == A64::Register{ insn.ops[0].reg }) {
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
				auto il_new = std::make_unique<LoadTaggedClassIdMayBeSmiInstr>(AddrRange(il_loadImm->Start(), insn.NextAddress()),
					std::unique_ptr<LoadValueInstr>(il_loadImm), std::unique_ptr<BranchIfSmiInstr>(il_branchIfSmi), 
					std::unique_ptr<LoadClassIdInstr>(il_loadClassId));
				fnInfo->il_insns.resize(fnInfo->il_insns.size() - 3);
				return ILResult{ insn.ptr(), std::move(il_new) };
			}
		}
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processBoxInt64Instr(AsmInstruction insn)
{
	// From BoxInt64Instr::EmitNativeCode()
	// Note: without compressed pointer, start instruction is "adds"
	// 0x21c5e8: sbfiz  x0, x2, #1, #0x1f
	// 0x21c5ec: cmp  x2, x0, asr #1
	// 0x21c5f0: b.eq  #0x21c5fc
	// 0x21c5f4: bl  #0x2e5500  # AllocateMintSharedWithoutFPURegsStub
	// 0x21c5f8: stur  x2, [x0, #7]
	//   x0 is result object (can be Smi or Mint). x2 is source integer.
	if (insn.id() == ARM64_INS_SBFIZ && insn.ops[2].imm == dart::kSmiTagSize && insn.ops[3].imm == 31) {
		auto insn0 = insn.ptr();
		const auto out_reg = insn.ops[0].reg;
		const auto in_reg = insn.ops[1].reg;

		++insn;
		if (insn.id() == ARM64_INS_CMP && insn.ops[0].reg == in_reg && insn.ops[1].reg == out_reg && insn.ops[1].shift.type == ARM64_SFT_ASR && insn.ops[1].shift.value == dart::kSmiTagSize) {
			// branch if integer value is fit in 31 bits
			++insn;
			ASSERT(insn.id() == ARM64_INS_B && insn.cc() == ARM64_CC_EQ);
			const auto contAddr = insn.ops[0].imm;

			++insn;
			ASSERT(insn.id() == ARM64_INS_BL);
			auto stub = app.GetFunction(insn.ops[0].imm);
			ASSERT(stub->IsStub());
			const auto stubKind = reinterpret_cast<DartStub*>(stub)->kind;
			INSN_ASSERT(stubKind == DartStub::AllocateMintSharedWithoutFPURegsStub || stubKind == DartStub::AllocateMintSharedWithFPURegsStub);

			++insn;
			INSN_ASSERT(insn.id() == ARM64_INS_STUR);
			INSN_ASSERT(insn.ops[0].reg == in_reg);
			INSN_ASSERT(insn.ops[1].mem.base == out_reg && insn.ops[1].mem.disp == dart::Mint::value_offset() - dart::kHeapObjectTag);

			const auto nextAddr = insn.NextAddress();
			INSN_ASSERT(nextAddr == contAddr);

			const auto objReg = A64::Register{ out_reg };
			const auto srcReg = A64::Register{ in_reg };
			return ILResult{ insn.ptr(), std::make_unique<BoxInt64Instr>(AddrRange(insn0->address, nextAddr), objReg, srcReg) };
		}
		else {
			--insn;
		}
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processLoadInt32FromBoxOrSmiInstr(AsmInstruction insn)
{
	// From UnboxInstr::EmitLoadInt32FromBoxOrSmi(), includes branch if Smi
	//   output is raw integer, input is Dart object
	if (insn.id() == ARM64_INS_SBFX && insn.ops[2].imm == dart::kSmiTagSize && insn.ops[3].imm == 31) {
		const auto insn0 = insn.ptr();
		const auto out_reg = insn.ops[0].reg;
		const auto in_reg = insn.ops[1].reg;
		const auto dstReg = A64::Register{ out_reg };
		const auto srcReg = A64::Register{ in_reg };
		// TODO: make srcReg type in current function register to be Smi

		++insn;
		if (insn.id() == ARM64_INS_TBZ && A64::Register{ insn.ops[0].reg } == srcReg && insn.ops[1].imm == dart::kSmiTag) {
			// if not smi, get raw integer from Mint object
			// these instructions might not be here because compiler know the value is always be Smi
			const auto cont_addr = insn.ops[2].imm;

			++insn;
			INSN_ASSERT(insn.id() == ARM64_INS_LDUR);
			INSN_ASSERT(insn.ops[0].reg == out_reg);
			INSN_ASSERT(insn.ops[1].mem.base == in_reg && insn.ops[1].mem.disp == dart::Mint::value_offset() - dart::kHeapObjectTag);

			// srcReg item type MUST be Mint

			INSN_ASSERT(insn.NextAddress() == cont_addr);
		}
		else {
			--insn;
			// srcReg object is always Smi
		}

		return ILResult{ insn.ptr(), std::make_unique<LoadInt32Instr>(AddrRange(insn0->address, insn.NextAddress()), dstReg, srcReg) };
	}
	return ILResult{};
}

ILResult FunctionAnalyzer::processTryAllocateObject(AsmInstruction insn)
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
	if (insn.id() == ARM64_INS_LDP && insn.ops[2].mem.base == CSREG_DART_THR && insn.ops[2].mem.disp == dart::Thread::top_offset()) {
		const auto insn0_addr = insn.address();

		const auto inst_reg = insn.ops[0].reg;
		const auto temp_reg = insn.ops[1].reg;

		++insn;
		INSN_ASSERT(insn.id() == ARM64_INS_ADD);
		INSN_ASSERT(insn.ops[0].reg == inst_reg);
		INSN_ASSERT(insn.ops[1].reg == inst_reg);
		const auto inst_size = insn.ops[2].imm;
		INSN_ASSERT(inst_size == 0x10);

		++insn;
		INSN_ASSERT(insn.id() == ARM64_INS_CMP);
		INSN_ASSERT(insn.ops[0].reg == temp_reg);
		INSN_ASSERT(insn.ops[1].reg == inst_reg);

		++insn;
		INSN_ASSERT(insn.id() == ARM64_INS_B && insn.cc() == ARM64_CC_LS);
		const uint64_t slow_path = (uint64_t)insn.ops[0].imm;
		// slow path is always at the end of function
		INSN_ASSERT(insn.address() < slow_path && slow_path < dartFn->AddressEnd());

		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_STR);
		INSN_ASSERT(insn.ops[0].reg == inst_reg);
		INSN_ASSERT(insn.ops[1].mem.base == CSREG_DART_THR && insn.ops[1].mem.disp == dart::Thread::top_offset());

		++insn;
		INSN_ASSERT(insn.id() == ARM64_INS_SUB);
		INSN_ASSERT(insn.ops[0].reg == inst_reg);
		INSN_ASSERT(insn.ops[1].reg == inst_reg);
		INSN_ASSERT(insn.ops[2].imm == inst_size - 1); // -1 because of kHeapObjectTag

		// also can reuse loadImm
		++insn;
		INSN_ASSERT(insn.IsMovz());
		INSN_ASSERT(insn.ops[0].reg == temp_reg);
		auto tag = insn.ops[1].imm;

		++insn;
		// movk instruction is needed with bits 16-32 is not zero (class id > 16)
		if (insn.id() == ARM64_INS_MOVK) {
			INSN_ASSERT(insn.ops[0].reg == temp_reg);
			INSN_ASSERT(insn.ops[1].shift.value == 16);
			tag |= (insn.ops[1].imm << 16);
			++insn;
		}

		INSN_ASSERT(insn.id() == ARM64_INS_STUR);
		INSN_ASSERT(insn.ops[0].reg == temp_reg);
		INSN_ASSERT(insn.ops[1].mem.base == inst_reg && insn.ops[1].mem.disp == -1); // because of kHeapObjectTag

		const uint32_t cid = (tag >> 12) & 0xfffff;
		auto dartCls = app.GetClass(cid);
		//INSN_ASSERT(dartCls->Size() < 0 || dartCls->Size() == inst_size);

		const auto dstReg = A64::Register{ inst_reg };

		// TODO: extra check by walk through slow path

		return ILResult{ insn.ptr(), std::make_unique<AllocateObjectInstr>(AddrRange(insn0_addr, insn.NextAddress()), dstReg, *dartCls) };
	}

	return ILResult{};
}

ILWBResult FunctionAnalyzer::processWriteBarrier(AsmInstruction insn)
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
	const auto ins0_addr = insn.address();
	//const auto objReg = A64::FromDartReg(dart::kWriteBarrierObjectReg);
	//const auto valReg = A64::FromDartReg(dart::kWriteBarrierValueReg);
	A64::Register objReg;
	A64::Register valReg;
	uint64_t contSmiAddr = 0;
	if (insn.id() == ARM64_INS_TBZ) {
		if (insn.ops[1].imm != dart::kSmiTag)
			return ILWBResult{};
		valReg = A64::Register{ insn.ops[0].reg };
		contSmiAddr = (uint64_t)insn.ops[2].imm;
		++insn;
	}

	if (insn.id() != ARM64_INS_LDURB || A64::Register{ insn.ops[0].reg } != A64::TMP_REG || insn.ops[1].mem.disp != -1)
		return ILWBResult{}; // it is not WriteBarrier
	objReg = A64::Register{ insn.ops[1].mem.base };

	++insn;
	if (insn.id() != ARM64_INS_LDURB || A64::Register{ insn.ops[0].reg } != A64::TMP2_REG || insn.ops[1].mem.disp != -1)
		return ILWBResult{};
	if (valReg != A64::Register::kNoRegister) {
		INSN_ASSERT(A64::Register{ insn.ops[1].mem.base } == valReg);
	}
	else {
		valReg = A64::Register{ insn.ops[1].mem.base };
	}

	++insn;
	INSN_ASSERT(insn.id() == ARM64_INS_AND);
	INSN_ASSERT(A64::Register{ insn.ops[0].reg } == A64::TMP_REG);
	INSN_ASSERT(A64::Register{ insn.ops[1].reg } == A64::TMP2_REG);
	INSN_ASSERT(A64::Register{ insn.ops[2].reg } == A64::TMP_REG);
	INSN_ASSERT(insn.ops[2].shift.type == ARM64_SFT_LSR && insn.ops[2].shift.value == 2);

	++insn;
	INSN_ASSERT(insn.id() == ARM64_INS_TST);
	INSN_ASSERT(insn.ops[0].reg == CSREG_DART_TMP);
	INSN_ASSERT(insn.ops[1].reg == CSREG_DART_HEAP);
	INSN_ASSERT(insn.ops[1].shift.type == ARM64_SFT_LSR && insn.ops[1].shift.value == 32);

	++insn;
	INSN_ASSERT(insn.id() == ARM64_INS_B && insn.cc() == ARM64_CC_EQ);
	const auto contAddr = (uint64_t)insn.ops[0].imm;
	INSN_ASSERT(contSmiAddr == 0 || contSmiAddr == contAddr);

	++insn;
	bool spill_lr = false;
	if (insn.id() == ARM64_INS_STR) {
		// spill_lr => Push(LR)
		// 0x1d5f58: str  x30, [x15, #-8]!
		INSN_ASSERT(insn.writeback());
		INSN_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
		INSN_ASSERT(insn.ops[1].mem.base == CSREG_DART_SP && insn.ops[1].mem.disp == -8);
		spill_lr = true;
		++insn;
	}

	// TODO: call instruction is not processed. AsmText cannot show the stub name.
	bool isArray;
	if (insn.id() == ARM64_INS_BL) {
		auto stub = app.GetFunction(insn.ops[0].imm);
		INSN_ASSERT(stub->IsStub());
		const auto stubKind = reinterpret_cast<DartStub*>(stub)->kind;
		INSN_ASSERT(stubKind == DartStub::WriteBarrierWrappersStub || stubKind == DartStub::ArrayWriteBarrierStub);
		isArray = stubKind == DartStub::ArrayWriteBarrierStub;
	}
	else {
		// from generate_invoke_write_barrier_wrapper_ lambda function
		// - write_barrier_wrappers_thread_offset(reg) in thread.h calculates thread offset from a register
		// Call(Address target) always use LR register
		INSN_ASSERT(insn.id() == ARM64_INS_LDR);
		INSN_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
		INSN_ASSERT(insn.ops[1].mem.base == CSREG_DART_THR);
		const auto existed = std::find(std::begin(AOT_Thread_write_barrier_wrappers_thread_offset), 
			std::end(AOT_Thread_write_barrier_wrappers_thread_offset), insn.ops[1].mem.disp) != std::end(AOT_Thread_write_barrier_wrappers_thread_offset);
		INSN_ASSERT(existed);
		const auto epReg = insn.ops[0].reg;
		++insn;

		INSN_ASSERT(insn.id() == ARM64_INS_BLR);
		INSN_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
		isArray = false;
	}

	if (spill_lr) {
		++insn;
		// 0x1d5f60: ldr  x30, [x15], #8
		INSN_ASSERT(insn.id() == ARM64_INS_LDR && insn.writeback());
		INSN_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
		INSN_ASSERT(insn.ops[1].mem.base == CSREG_DART_SP);
		INSN_ASSERT(insn.ops[2].imm == 8);
	}

	INSN_ASSERT(insn.NextAddress() == contAddr);

	return ILWBResult{ insn.ptr(), std::make_unique<WriteBarrierInstr>(AddrRange(ins0_addr, insn.NextAddress()), objReg, valReg, isArray) };
}

ILResult FunctionAnalyzer::processWriteBarrierInstr(AsmInstruction insn)
{
	auto res = processWriteBarrier(insn);
	return ILResult{ res.lastIns, std::move(res.il) };
}

static ArrayOp getArrayOp(AsmInstruction insn)
{
	ArrayOp op;
	if (insn.writeback())
		return ArrayOp();
	switch (insn.id()) {
	case ARM64_INS_LDUR: {
		// for 64 bit integer, LDUR is used too
		const auto regSize = GetCsRegSize(insn.ops[0].reg);
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
		const auto regSize = GetCsRegSize(insn.ops[0].reg);
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

ILResult FunctionAnalyzer::processLoadStore(AsmInstruction insn)
{
	// handles load/store array element by calculating offset first (index as register)
	// Dart use only ADD instruction for calculating memory offset
	if (insn.id() == ARM64_INS_ADD) {
		auto ins0_addr = insn.address(); // save first instruction pointer
		// tmp register for storing memory offset is TMP or R25 as kWriteBarrierSlotReg (in case of StoreBerrier is needed)
		if (insn.ops[0].reg == CSREG_DART_WB_SLOT) {
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
			if (insn.ops[1].reg != CSREG_DART_WB_OBJECT)
				return ILResult{}; // not array load/store

			const auto objReg = A64::Register{ dart::kWriteBarrierObjectReg };
			const auto valReg = A64::Register{ dart::kWriteBarrierValueReg };
			// TODO: objReg type MUST be Dart array
			auto idx = VarStorage::NewSmallImm(0);
			if (insn.ops[2].type == ARM64_OP_IMM) {
				// const int64_t offset = index * index_scale + HeapDataOffset(is_external, cid);
				const auto arr_idx = (insn.ops[2].imm + dart::kHeapObjectTag - dart::Array::data_offset()) / dart::kCompressedWordSize;
				if (arr_idx < 0)
					return ILResult{};
				idx = VarStorage::NewSmallImm(arr_idx);
			}
			else {
				// register as index
				INSN_ASSERT(insn.ops[2].shift.type == ARM64_SFT_LSL && 
					(insn.ops[2].shift.value == dart::kCompressedWordSizeLog2 || 
						(insn.ops[2].shift.value == dart::kCompressedWordSizeLog2 - 1 || insn.ops[2].ext == ARM64_EXT_SXTW)));
				idx = VarStorage(A64::Register{ insn.ops[2].reg });

				++insn;
				INSN_ASSERT(insn.id() == ARM64_INS_ADD);
				INSN_ASSERT(insn.ops[0].reg == CSREG_DART_WB_SLOT);
				INSN_ASSERT(insn.ops[1].reg == CSREG_DART_WB_SLOT);
				INSN_ASSERT(insn.ops[2].imm == dart::Array::data_offset() - dart::kHeapObjectTag);
			}

			++insn;
			INSN_ASSERT(insn.id() == ARM64_INS_STR);
			INSN_ASSERT(A64::Register{ insn.ops[0].reg } == valReg && GetCsRegSize(insn.ops[0].reg) == dart::kCompressedWordSize);
			INSN_ASSERT(insn.ops[1].mem.base == CSREG_DART_WB_SLOT && insn.ops[1].mem.disp == 0);

			++insn;
			auto ilres = processWriteBarrier(insn);
			INSN_ASSERT(ilres.il);
			INSN_ASSERT(ilres.il->isArray);
			INSN_ASSERT(ilres.il->objReg == objReg && ilres.il->valReg == valReg);

			ArrayOp arrayOp(dart::kCompressedWordSize, false, ArrayOp::List);
			return ILResult{ ilres.lastIns, std::make_unique<StoreArrayElementInstr>(AddrRange(ins0_addr, ilres.NextAddress()), valReg, objReg, idx, arrayOp) };
		}

		// calculating memory offset (array index) should be from register only
		// tmp register size is always 8 (for 64 bits)
		if (insn.ops[2].type == ARM64_OP_REG && insn.ops[1].reg != CSREG_DART_FP && GetCsRegSize(insn.ops[0].reg) == 8) {
			const auto tmpReg = insn.ops[0].reg;
			// LoadIndexedInstr::EmitNativeCode() or StoreIndexedInstr::EmitNativeCode() non barrier path
			// 0x220460: add  x16, x3, x5, lsl #2
			// 0x220464: ldur  w1, [x16, #0xf]
			//  ===
			// 0x2c3958: add  x16, x2, x5
			// 0x2c395c: ldrb  w7, [x16, #0x17]  ; Uint8List
			//  ===
			// 0x40b868: add     x16, x1, w0, sxtw #1 ; w0 is Smi, so shift only 1 instead of 2
			// 0x40b86c: ldursw  x1, [x16, #0x17]  ; Int32List
			const auto arrReg = A64::Register{ insn.ops[1].reg };
			const auto idx = VarStorage(A64::Register{ insn.ops[2].reg });
			const auto shift = insn.ops[2].shift;
			if (shift.type != ARM64_SFT_INVALID && shift.type != ARM64_SFT_LSL)
				return ILResult{};
			const auto ext = insn.ops[2].ext;

			++insn;
			// array offset for index as register always be start of payload
			const auto arr_data_offset = insn.ops[1].mem.disp;
			if (insn.ops[1].mem.base != tmpReg || arr_data_offset < 8)
				return ILResult{};
			const auto arrayOp = getArrayOp(insn);
			if (!arrayOp.IsArrayOp())
				return ILResult{};
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
			if (arrayOp.isLoad) {
				// load always uses TMP register
				INSN_ASSERT(tmpReg == CSREG_DART_TMP);
				const auto dstReg = A64::Register{ insn.ops[0].reg };
				return ILResult{ insn.ptr(), std::make_unique<LoadArrayElementInstr>(AddrRange(ins0_addr, insn.NextAddress()), dstReg, arrReg, idx, arrayOp) };
			}
			else {
				// store operation
				const auto valReg = A64::Register{ insn.ops[0].reg };
				return ILResult{ insn.ptr(), std::make_unique<StoreArrayElementInstr>(AddrRange(ins0_addr, insn.NextAddress()), valReg, arrReg, idx, arrayOp) };
			}
		}
	}

	if (insn.ops[1].mem.base != CSREG_DART_FP && insn.ops[1].mem.disp != 0) {
		// load/store with fixed offset
		const auto arrayOp = getArrayOp(insn);
		if (arrayOp.IsArrayOp()) {
			ILResult res{};
			const auto valReg = A64::Register{ insn.ops[0].reg };
			const auto objReg = A64::Register{ insn.ops[1].mem.base };
			const auto offset = insn.ops[1].mem.disp;
			if (arrayOp.arrType == ArrayOp::Unknown) {
				// might be array or object. set it as object first.
				if (arrayOp.isLoad) {
					res.il = std::make_unique<LoadFieldInstr>(insn.ptr(), valReg, objReg, offset);
					res.lastIns = insn.ptr();
				}
				else {
					auto insn2 = insn.Next();
					const auto wbres = processWriteBarrier(insn2);
					if (wbres.il) {
						// with WriteBearrier, we can determine if the object is array or not
						// but it is still difficult if it is list or typed data
						INSN_ASSERT(wbres.il->objReg == objReg && wbres.il->valReg == valReg);
						res.lastIns = wbres.lastIns;
						if (wbres.il->isArray)
							res.il = std::make_unique<StoreArrayElementInstr>(AddrRange(insn.address(), res.NextAddress()), valReg, objReg, VarStorage::NewSmallImm(offset), arrayOp);
						else
							res.il = std::make_unique<StoreFieldInstr>(AddrRange(insn.address(), res.NextAddress()), valReg, objReg, offset);
					}
					else {
						res.il = std::make_unique<StoreFieldInstr>(insn.ptr(), valReg, objReg, offset);
						res.lastIns = insn.ptr();
					}
				}
			}
			else {
				// array
				const auto idx = VarStorage::NewSmallImm((offset + dart::kHeapObjectTag - dart::UntaggedTypedData::payload_offset()) / arrayOp.size);
				if (arrayOp.isLoad) {
					res.il = std::make_unique<LoadArrayElementInstr>(AddrRange(insn.address(), insn.NextAddress()), valReg, objReg, idx, arrayOp);
					res.lastIns = insn.ptr();
				}
				else {
					auto insn2 = insn.Next();
					const auto wbres = processWriteBarrier(insn2);
					if (wbres.il) {
						res.lastIns = wbres.lastIns;
						res.il = std::make_unique<StoreArrayElementInstr>(AddrRange(insn.address(), res.NextAddress()), valReg, objReg, idx, arrayOp);
					}
					else {
						res.il = std::make_unique<StoreArrayElementInstr>(AddrRange(insn.address(), insn.NextAddress()), valReg, objReg, idx, arrayOp);
						res.lastIns = insn.ptr();
					}
				}
			}

			return res;
		}
	}

	return ILResult{};
}

void FunctionAnalyzer::asm2il()
{
	auto last_insn = asm_insns.LastPtr();

	auto insn = asm_insns.FirstPtr();
	cs_insn* insn2;
	do {
		insn2 = nullptr;

		try {
			//for (auto matcher : matchers) {
			for (auto matcher : matcherFns) {
				//insn2 = matcher(fnInfo, AsmInstruction(insn));
				auto res = std::invoke(matcher, this, insn);
				insn2 = res.lastIns;
				if (insn2) {
					//auto il = fnInfo->il_insns.back().get();
					auto il = res.il.get();
					if (il->Kind() == ILInstr::LoadObject) {
						auto& asm_text = fnInfo->asmTexts.AtAddr(il->Start());
						const auto val = reinterpret_cast<LoadObjectInstr*>(il)->GetValue();
						if (val->Storage().kind == VarStorage::Pool) {
							// TODO: NativeDouble and NativeInt
							asm_text.dataType = AsmText::PoolOffset;
							asm_text.poolOffset = val->Storage().offset;
						}
						else if (val->Storage().kind == VarStorage::Immediate) {
							// can be only boolean. imm as integer or double is in pool offset
							RELEASE_ASSERT(val->Value()->TypeId() == dart::kBoolCid);
							asm_text.dataType = AsmText::Boolean;
							asm_text.boolVal = reinterpret_cast<VarBoolean*>(val->Value().get())->val;
						}
					}
					else if (il->Kind() == ILInstr::Call) {
						auto& asm_text = fnInfo->asmTexts.AtAddr(il->Start());
						asm_text.dataType = AsmText::Call;
						asm_text.callAddress = reinterpret_cast<CallInstr*>(il)->GetCallAddress();
					}

					fnInfo->AddInstruction(std::move(res.il));
					break;
				}
			}
		}
		catch (InsnException& e) {
			std::cerr << "Analysis error at line " << e.location.line()
				<< " `" << e.location.function_name() << "`: " << e.cond << '\n';
			const uint64_t fn_addr = fnInfo->dartFn.Address();
			auto ins = e.ins;
			for (int i = 0; ins->address > fn_addr && i < 4; i++) {
				--ins;
			}
			while (ins != e.ins) {
				std::cerr << std::format("    {:#x}: {} {}\n", ins->address, &ins->mnemonic[0], &ins->op_str[0]);
				++ins;
			}
			std::cerr << std::format("  * {:#x}: {} {}\n", ins->address, &ins->mnemonic[0], &ins->op_str[0]);
			if (ins->address + ins->size < fnInfo->dartFn.AddressEnd()) {
				++ins;
				std::cerr << std::format("    {:#x}: {} {}\n", ins->address, &ins->mnemonic[0], &ins->op_str[0]);
			}
		}

		if (insn2 == nullptr) {
			// unhandle case
			fnInfo->AddInstruction(std::make_unique<UnknownInstr>(insn, fnInfo->asmTexts.AtAddr(insn->address)));
			insn2 = insn;
		}

		insn = insn2 + 1;
	} while (insn2 != last_insn);
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

	return asm_texts;
}

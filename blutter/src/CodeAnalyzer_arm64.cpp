#include "pch.h"
#include "CodeAnalyzer.h"
#include "DartApp.h"
#include "VarValue.h"

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
			return new VarInteger(dart::kSmiCid, dart::RawSmiValue(dart::Smi::RawCast(ptr)));
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
			return new VarInteger(dart::kSmiCid, dart::Smi::Cast(obj).Value());
		case dart::kMintCid:
			return new VarInteger(dart::kMintCid, dart::Mint::Cast(obj).AsInt64Value());
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
		case dart::kTypeParametersCid:
			throw std::runtime_error("Type parameter in Object Pool");
		case dart::kTypeCid:
			return new VarType(*app.TypeDb()->FindOrAdd(dart::Type::Cast(obj).ptr()));
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
		if (A64::IsDecimalRegister(dstReg))
			return new VarDouble(*((double*)&imm));
		return new VarInteger(dart::kIntegerCid, imm);
	}
	else if (objType == dart::ObjectPool::EntryType::kNativeFunction) {
		//val = pool.RawValueAt(idx);
		throw std::runtime_error("getting native function pool object from Dart code");
	}
	else {
		throw std::runtime_error(std::format("unknown pool object type: {}", (int)objType).c_str());
	}
}

std::tuple<int, A64::Register, std::shared_ptr<VarItem>> processGetObjectPoolInstruction(DartApp& app, AsmInstruction insn)
{
	int64_t offset = 0;
	int insCnt = 0;
	A64::Register dstReg;
	//auto ins_ptr = insn.ptr();
	if (insn.id() == ARM64_INS_LDR) {
		// PP offset less than 12 bits
		if (insn.ops[1].mem.base == CSREG_DART_PP && insn.ops[1].mem.index == ARM64_REG_INVALID) {
			offset = insn.ops[1].mem.disp;
			dstReg = A64::FromCsReg(insn.ops[0].reg);
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
			dstReg = A64::FromCsReg(insn.ops[0].reg);
			auto item = std::make_shared<VarItem>(VarStorage::NewImmediate(), new VarBoolean(val));
			return { 1, dstReg, item };
		}

		if (insn.ops[1].reg == CSREG_DART_PP && insn.ops[2].type == ARM64_OP_IMM && insn.ops[2].shift.type == ARM64_SFT_LSL && insn.ops[2].shift.value == 12) {
			auto base = insn.ops[2].imm << 12;
			auto offset_reg = insn.ops[0].reg;

			++insn;
			if (insn.id() == ARM64_INS_LDR) {
				RELEASE_ASSERT(insn.ops[1].mem.base == offset_reg);
				offset = base + insn.ops[1].mem.disp;
			}
			else if (insn.id() == ARM64_INS_LDP) {
				RELEASE_ASSERT(insn.ops[2].mem.base == offset_reg);
				offset = base + insn.ops[2].mem.disp;
			}
			else if (insn.id() == ARM64_INS_ADD) {
				// use when loading pair from object pool (local 2 entries)
				// see it for UnlinkedCall by the next entry is the jump address
				RELEASE_ASSERT(insn.ops[2].type == ARM64_OP_IMM);
				offset = base + insn.ops[2].imm;
			}
			dstReg = A64::FromCsReg(insn.ops[0].reg);
			insCnt = 2;
		}
	}
	else if (insn.id() == ARM64_INS_MOV) {
		// more than 20 bits (never seen it)
		// MOV X5, #offset_low
		// MOVK X5, #offset_high LSL#16
		// LDR X6, [X27,X5]
		auto offset_reg = insn.ops[0].reg;
		offset = insn.ops[1].imm;

		++insn;
		if (insn.id() == ARM64_INS_MOVK && insn.ops[0].reg == offset_reg && 
			insn.ops[1].type == ARM64_OP_IMM && insn.ops[1].shift.type == ARM64_SFT_LSL && insn.ops[1].shift.value == 16)
		{
			offset |= insn.ops[1].imm << 16;

			++insn;
			if (insn.id() == ARM64_INS_LDR && insn.ops[1].mem.base == CSREG_DART_PP && insn.ops[1].mem.index == offset_reg) {
				dstReg = A64::FromCsReg(insn.ops[0].reg);
				insCnt = 3;
			}
		}
	}

	if (insCnt == 0) {
		return { 0, A64::Register::kNoRegister, nullptr };
	}

	auto val = getPoolObject(app, offset, dstReg);
	auto item = std::make_shared<VarItem>(VarStorage::NewPool((int)offset), val);
	return { insCnt, dstReg, item };
}

static cs_insn* processEnterFrameInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_STP) {
		if (insn.ops[0].reg == CSREG_DART_FP && insn.ops[1].reg == ARM64_REG_LR && insn.ops[2].mem.base == CSREG_DART_SP) {
			RELEASE_ASSERT(insn.writeback());
			auto insn0 = insn.ptr();
			++insn;
			RELEASE_ASSERT(insn.id() == ARM64_INS_MOV);
			RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_FP);
			RELEASE_ASSERT(insn.ops[1].reg == CSREG_DART_SP);
			fnInfo->useFramePointer = true;
			fnInfo->AddInstruction(std::make_unique<EnterFrameInstr>(AddrRange(insn0->address, insn.NextAddress())));
			return insn.ptr();
		}
	}
	return nullptr;
}

static cs_insn* processLeaveFrameInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_MOV) {
		if (insn.ops[0].reg == CSREG_DART_SP && insn.ops[1].reg == CSREG_DART_FP) {
			RELEASE_ASSERT(fnInfo->useFramePointer);
			auto insn0 = insn.ptr();
			++insn;
			RELEASE_ASSERT(insn.id() == ARM64_INS_LDP && insn.op_count() == 4);
			RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_FP);
			RELEASE_ASSERT(insn.ops[1].reg == ARM64_REG_LR);
			RELEASE_ASSERT(insn.ops[2].mem.base == CSREG_DART_SP);
			RELEASE_ASSERT(insn.ops[3].imm == 0x10);
			fnInfo->AddInstruction(std::make_unique<LeaveFrameInstr>(AddrRange(insn0->address, insn.NextAddress())));
			return insn.ptr();
		}
	}
	return nullptr;
}

static cs_insn* processAllocateStackInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_SUB) {
		if (insn.ops[0].reg == CSREG_DART_SP && insn.ops[1].reg == CSREG_DART_SP && insn.ops[2].type == ARM64_OP_IMM) {
			const auto stackSize = (uint32_t)insn.ops[2].imm;
			fnInfo->stackSize = stackSize;
			fnInfo->AddInstruction(std::make_unique<AllocateStackInstr>(insn.ptr(), stackSize));
			return insn.ptr();
		}
	}
	return nullptr;
}

static cs_insn* processCheckStackOverflowInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_LDR) {
		if (insn.ops[1].mem.base == CSREG_DART_THR && insn.ops[1].mem.disp == AOT_Thread_stack_limit_offset) {
			RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_TMP);
			auto insn0 = insn.ptr();
			++insn;

			// cmp SP, TMP
			RELEASE_ASSERT(insn.id() == ARM64_INS_CMP);
			RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_SP);
			RELEASE_ASSERT(insn.ops[1].reg == CSREG_DART_TMP);
			++insn;

			// b.ls #overflow_target
			RELEASE_ASSERT(insn.IsBranch(ARM64_CC_LS));
			RELEASE_ASSERT(insn.ops[0].type == ARM64_OP_IMM);
			uint64_t target = (uint64_t)insn.ops[0].imm;
			// the dart compiler always put slow path at the end of function after "ret"
			RELEASE_ASSERT(target < fnInfo->dartFn.AddressEnd() && target >= insn.address());
			fnInfo->AddInstruction(std::make_unique<CheckStackOverflowInstr>(AddrRange(insn0->address, insn.NextAddress()), target));
			return insn.ptr();
		}
	}
	return nullptr;
}

static cs_insn* processLoadPoolObjectInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	auto [insCnt, ppObjReg, ppObjItem] = processGetObjectPoolInstruction(fnInfo->app, insn);
	if (insCnt > 0) {
		auto insn0 = insn.ptr();
		insn += insCnt - 1;
		fnInfo->AddInstruction(std::make_unique<LoadObjectInstr>(AddrRange(insn0->address, insn.NextAddress()), ppObjReg, ppObjItem));
		// TODO: load object might be start of Dart instruction, check next instruction
		return insn.ptr();
	}
	return nullptr;
}

static cs_insn* processDecompressPointerInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_ADD) {
		if (insn.ops[2].reg == CSREG_DART_HEAP && insn.ops[2].shift.value == 32) {
			RELEASE_ASSERT(insn.ops[0].reg == insn.ops[1].reg);
			fnInfo->AddInstruction(std::make_unique<DecompressPointerInstr>(insn.ptr(), A64::FromCsReg(insn.ops[0].reg)));
			return insn.ptr();
		}
	}
	return nullptr;
}

static cs_insn* processCallInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_BL) {
		if (insn.ops[0].type == ARM64_OP_IMM) {
			const auto target = insn.ops[0].imm;
			fnInfo->AddInstruction(std::make_unique<CallInstr>(insn.ptr(), fnInfo->app.GetFunction(target)));
			return insn.ptr();
		}
		// should indirect call handle here
	}
	else if (insn.id() == ARM64_INS_B && insn.ops[0].type == ARM64_OP_IMM) {
		// this might tail branch (function call at the end of function)
		const auto& dartFn = fnInfo->dartFn;
		const auto target = insn.ops[0].imm;
		if (target < dartFn.Address() || target >= dartFn.AddressEnd()) {
			fnInfo->AddInstruction(std::make_unique<CallInstr>(insn.ptr(), fnInfo->app.GetFunction(target)));
			return insn.ptr();
		}
	}

	return nullptr;
}

static cs_insn* processGdtCallInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
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

	auto insn0 = insn.ptr();
	arm64_reg cidReg = ARM64_REG_INVALID;
	int64_t offset = 0;
	// expect offset less than 16 bits, libapp MUST be very large to make offset larger than 16 bits
	if (insn.id() == ARM64_INS_MOVZ && insn.ops[0].reg == CSREG_DART_TMP2) {
		offset = insn.ops[1].imm;
		const auto tmpReg = insn.ops[0].reg;
		++insn;

		if (insn.id() == ARM64_INS_MOVK && insn.ops[0].reg == tmpReg && insn.ops[1].shift.value == 16) {
			// huge app
			offset |= insn.ops[1].imm << 16;
			++insn;
		}
		if (insn.id() == ARM64_INS_ADD && insn.ops[0].reg == CSREG_DART_LR) {
			RELEASE_ASSERT(insn.ops[2].reg == tmpReg);
			cidReg = insn.ops[1].reg;
		}
	}
	else if (insn.id() == ARM64_INS_ORR && insn.ops[0].reg == CSREG_DART_TMP2 && insn.ops[1].reg == ARM64_REG_XZR && insn.ops[2].type == ARM64_OP_IMM) {
		offset = insn.ops[2].imm;
		const auto tmpReg = insn.ops[0].reg;
		++insn;

		if (insn.id() == ARM64_INS_ADD && insn.ops[0].reg == CSREG_DART_LR) {
			RELEASE_ASSERT(insn.ops[2].reg == tmpReg);
			cidReg = insn.ops[1].reg;
		}
	}
	else if (insn.ops[0].reg == CSREG_DART_LR && (insn.id() == ARM64_INS_ADD || insn.id() == ARM64_INS_SUB)) {
		ASSERT(insn.ops[2].type == ARM64_OP_IMM);
		cidReg = insn.ops[1].reg;
		offset = insn.ops[2].imm;
		if (insn.ops[2].shift.type != ARM64_SFT_INVALID) {
			ASSERT(insn.ops[2].shift.type == ARM64_SFT_LSL);
			offset <<= insn.ops[2].shift.value;
		}
		if (insn.id() == ARM64_INS_SUB)
			offset = -offset;
	}

	if (cidReg == ToCapstoneReg(dart::DispatchTableNullErrorABI::kClassIdReg)) {
		// this is GDT call
		//const auto selector_offset = dart::DispatchTable::kOriginElement + offset;
		++insn;
		RELEASE_ASSERT(insn.id() == ARM64_INS_LDR);
		RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
		ASSERT(insn.ops[1].mem.base == CSREG_DART_DISPATCH_TABLE && insn.ops[1].mem.index == CSREG_DART_LR && insn.ops[1].shift.value == 3);

		++insn;
		RELEASE_ASSERT(insn.id() == ARM64_INS_BLR);
		RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_LR);

		fnInfo->AddInstruction(std::make_unique<GdtCallInstr>(AddrRange(insn0->address, insn.NextAddress()), offset));
		return insn.ptr();
	}

	return nullptr;
}

static cs_insn* processReturnInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_RET) {
		fnInfo->AddInstruction(std::make_unique<ReturnInstr>(insn.ptr()));
		return insn.ptr();
	}
	return nullptr;
}

using MatcherFn = cs_insn* (*)(AnalyzedFnData* fnInfo, AsmInstruction insn);
static const MatcherFn matchers[] = {
	processEnterFrameInstr,
	processLeaveFrameInstr,
	processAllocateStackInstr,
	processCheckStackOverflowInstr,
	processLoadPoolObjectInstr,
	processDecompressPointerInstr,
	processCallInstr,
	processGdtCallInstr,
	processReturnInstr,
};

void CodeAnalyzer::asm2il(DartFunction* dartFn, AsmInstructions& asm_insns)
{
	auto fnInfo = dartFn->GetAnalyzedData();

	auto last_insn = asm_insns.LastPtr();

	auto insn = asm_insns.FirstPtr();
	cs_insn* insn2;
	do {
		insn2 = nullptr;

		for (auto matcher : matchers) {
			insn2 = matcher(fnInfo, AsmInstruction(insn));
			if (insn2) {
				auto& asm_text = fnInfo->asmTexts.AtAddr(insn->address);
				if (asm_text.dataType == AsmText::None) {
					asm_text.dataType = AsmText::Instruction;
					asm_text.insn = fnInfo->il_insns.back().get();
				}
				break;
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

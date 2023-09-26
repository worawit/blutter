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
		if (A64::IsDecimalRegister(dstReg))
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
			RELEASE_ASSERT(insn.id() == ARM64_INS_B);
			RELEASE_ASSERT(insn.ops[0].type == ARM64_OP_IMM);
			uint64_t target = 0;
			// b.ls #overflow_target
			if (insn.cc() == ARM64_CC_LS) {
				target = (uint64_t)insn.ops[0].imm;
			}
			else if (insn.cc() == ARM64_CC_HI) {
				const auto cont_target = insn.ops[0].imm;

				++insn;
				RELEASE_ASSERT(insn.IsBranch());
				RELEASE_ASSERT(insn.NextAddress() == cont_target);
				target = (uint64_t)insn.ops[0].imm;
			}
			else {
				FATAL("unexpect branch condition for CheckStackOverflow");
			}

			if (target != 0) {
				// the dart compiler always put slow path at the end of function after "ret"
				RELEASE_ASSERT(target < fnInfo->dartFn.AddressEnd() && target >= insn.address());
				fnInfo->AddInstruction(std::make_unique<CheckStackOverflowInstr>(AddrRange(insn0->address, insn.NextAddress()), target));
				return insn.ptr();
			}
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
			fnInfo->AddInstruction(std::make_unique<CallInstr>(insn.ptr(), fnInfo->app.GetFunction(target), (uint64_t)target));
			return insn.ptr();
		}
		// should indirect call handle here
	}
	else if (insn.id() == ARM64_INS_B && insn.ops[0].type == ARM64_OP_IMM) {
		// this might tail branch (function call at the end of function)
		const auto& dartFn = fnInfo->dartFn;
		const auto target = insn.ops[0].imm;
		if (target < dartFn.Address() || target >= dartFn.AddressEnd()) {
			fnInfo->AddInstruction(std::make_unique<CallInstr>(insn.ptr(), fnInfo->app.GetFunction(target), (uint64_t)target));
			return insn.ptr();
		}
	}

	return nullptr;
}

static cs_insn* processLoadFieldTableInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
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
		const auto dstReg = A64::FromCsReg(result_reg);
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

		RELEASE_ASSERT(insn.ops[1].mem.base == tmp_reg);
		load_offset |= insn.ops[1].mem.disp;
		const auto field_offset = load_offset >> 1;

		if (insn.id() == ARM64_INS_STR) {
			fnInfo->AddInstruction(std::make_unique<StoreStaticFieldInstr>(AddrRange(insn0_addr, insn.NextAddress()), A64::FromCsReg(insn.ops[0].reg), field_offset));
			return insn.ptr();
		}
		else {
			RELEASE_ASSERT(insn.ops[0].reg == result_reg);

			auto loadStaticInstr = std::make_unique<LoadStaticFieldInstr>(AddrRange(insn0_addr, insn.NextAddress()), dstReg, field_offset);
			auto loadStatic_last_ptr = insn.ptr();

			++insn;
			// load Sentinel from PP to check if this field is initialized?
			auto [loadPpInsCnt, ppObjReg, ppObjItem] = processGetObjectPoolInstruction(fnInfo->app, insn);
			if (loadPpInsCnt == 0 || ppObjReg != A64::TMP_REG || ppObjItem->Value()->RawTypeId() != dart::kSentinelCid) {
				// this is just load static field (same as global variable). cannot find the owner
				fnInfo->AddInstruction(std::move(loadStaticInstr));
				return loadStatic_last_ptr;
			}

			// if any condition is not met, fallback to just load static field
			insn += loadPpInsCnt;
			RELEASE_ASSERT(insn.id() == ARM64_INS_CMP);
			RELEASE_ASSERT(A64::FromCsReg(insn.ops[0].reg) == dstReg);
			RELEASE_ASSERT(A64::FromCsReg(insn.ops[1].reg) == A64::TMP_REG);

			++insn;
			RELEASE_ASSERT(insn.id() == ARM64_INS_B);
			if (insn.cc() == ARM64_CC_NE) {
				const auto cont_addr = insn.ops[0].imm;

				// get pool object
				++insn;
				auto [loadPpInsCnt, ppObjReg, ppObjItem] = processGetObjectPoolInstruction(fnInfo->app, insn);
				if (loadPpInsCnt > 0) {
					RELEASE_ASSERT(ppObjReg == A64::FromDartReg(dart::InitStaticFieldABI::kFieldReg));
					RELEASE_ASSERT(ppObjItem->Value()->RawTypeId() == dart::kFieldCid);
					auto& dartField = reinterpret_cast<VarField*>(ppObjItem->Value().get())->field;
					RELEASE_ASSERT(dartField.Offset() == field_offset);

					insn += loadPpInsCnt;
					RELEASE_ASSERT(insn.id() == ARM64_INS_BL);

					auto dartFn = fnInfo->app.GetFunction(insn.ops[0].imm);
					ASSERT(dartFn->IsStub());
					const auto stubKind = reinterpret_cast<DartStub*>(dartFn)->kind;
					RELEASE_ASSERT(stubKind == DartStub::InitLateStaticFieldStub || stubKind == DartStub::InitLateFinalStaticFieldStub);

					RELEASE_ASSERT(insn.NextAddress() == cont_addr);

					auto instr = std::make_unique<InitLateStaticFieldInstr>(AddrRange(insn0_addr, insn.NextAddress()), dstReg, dartField);
					fnInfo->AddInstruction(std::move(instr));

					return insn.ptr();
				}
			}
			else {
				// late intialize error
				RELEASE_ASSERT(insn.cc() == ARM64_CC_EQ);
				const auto error_addr = insn.ops[0].imm;
				RELEASE_ASSERT(error_addr > insn.NextAddress() && error_addr < fnInfo->dartFn.AddressEnd());
				
				auto insn2 = insn.MoveTo(error_addr);
				auto [loadPpInsCnt, ppObjReg, ppObjItem] = processGetObjectPoolInstruction(fnInfo->app, insn2);
				if (loadPpInsCnt > 0 && ppObjReg == A64::FromDartReg(dart::LateInitializationErrorABI::kFieldReg) && ppObjItem->Value()->RawTypeId() == dart::kFieldCid) {
					auto& dartField = reinterpret_cast<VarField*>(ppObjItem->Value().get())->field;
					RELEASE_ASSERT(dartField.Offset() == field_offset);

					insn2 += loadPpInsCnt;
					RELEASE_ASSERT(insn2.id() == ARM64_INS_BL && insn2.ops[0].type == ARM64_OP_IMM);
					auto fn = fnInfo->app.GetFunction(insn2.ops[0].imm);
					auto stub = fn->AsStub();
					RELEASE_ASSERT(stub->kind == DartStub::LateInitializationErrorSharedWithoutFPURegsStub || stub->kind == DartStub::LateInitializationErrorSharedWithFPURegsStub);
					// load static field but throw an exception if it is not initialized
					fnInfo->AddInstruction(std::make_unique<LoadStaticFieldInstr>(AddrRange(insn0_addr, insn.NextAddress()), dstReg, field_offset));
					return insn.ptr();
				}
			}

			// this one might be for if/else when Static field is not initialized
			fnInfo->AddInstruction(std::move(loadStaticInstr));
			return loadStatic_last_ptr;
		}
	}

	return nullptr;
}

static cs_insn* processLoadImmInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
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
		fnInfo->AddInstruction(std::make_unique<LoadImmInstr>(AddrRange(insn0->address, insn.NextAddress()), A64::FromCsReg(dstReg), imm));
		return insn.ptr();
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
			RELEASE_ASSERT(insn.id() == ARM64_INS_ADD);
			RELEASE_ASSERT(insn.ops[2].type == ARM64_OP_REG && insn.ops[2].reg == CSREG_DART_TMP2);

			auto& il = fnInfo->il_insns.back();
			auto il_loadImm = reinterpret_cast<LoadImmInstr*>(il.get());
			RELEASE_ASSERT(il_loadImm->dstReg == A64::TMP2_REG);
			offset = il_loadImm->val;
			insn0_addr = il->Start();
			// delete the last imm IL
			fnInfo->il_insns.pop_back();
		}
		//const auto selector_offset = dart::DispatchTable::kOriginElement + offset;
		++insn;
		RELEASE_ASSERT(insn.id() == ARM64_INS_LDR);
		RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
		ASSERT(insn.ops[1].mem.base == CSREG_DART_DISPATCH_TABLE && insn.ops[1].mem.index == CSREG_DART_LR && insn.ops[1].shift.value == 3);

		++insn;
		RELEASE_ASSERT(insn.id() == ARM64_INS_BLR);
		RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_LR);

		fnInfo->AddInstruction(std::make_unique<GdtCallInstr>(AddrRange(insn0_addr, insn.NextAddress()), offset));
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

static cs_insn* processBranchIfSmiInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_TBZ && insn.ops[1].imm == dart::kSmiTag && dart::kCompressedWordSize == GetCsRegSize(insn.ops[0].reg)) {
		const auto objReg = A64::FromCsReg(insn.ops[0].reg);
		const auto branchAddr = insn.ops[2].imm;
		fnInfo->AddInstruction(std::make_unique<BranchIfSmiInstr>(insn.ptr(), objReg, branchAddr));
		return insn.ptr();
	}
	return nullptr;
}

static cs_insn* processLoadClassIdInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	if (insn.id() == ARM64_INS_LDUR && insn.ops[1].mem.disp == -1) {
		auto insn0 = insn.ptr();
		// 0x21cb10: ldur  x1, [x0, #-1]  ; load object tag
		const auto objReg = A64::FromCsReg(insn.ops[1].mem.base);
		const auto cidReg = insn.ops[0].reg;

		++insn;
		// Assembler::ExtractClassIdFromTags()
		// 0x21cb14: ubfx  x1, x1, #0xc, #0x14  ; extract object class id
		RELEASE_ASSERT(insn.id() == ARM64_INS_UBFX);
		RELEASE_ASSERT(insn.ops[0].reg == cidReg);
		RELEASE_ASSERT(insn.ops[1].reg == cidReg);
		RELEASE_ASSERT(insn.ops[2].imm == dart::UntaggedObject::kClassIdTagPos);
		RELEASE_ASSERT(insn.ops[3].imm == dart::UntaggedObject::kClassIdTagSize);
		fnInfo->AddInstruction(std::make_unique<LoadClassIdInstr>(AddrRange(insn0->address, insn.NextAddress()), objReg, A64::FromCsReg(cidReg)));
		return insn.ptr();
	}
	return nullptr;
}

static cs_insn* processLoadTaggedClassIdMayBeSmiInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	//   cidReg = SmiTaggedClassId
	//   branchIfSmi(objReg, branchAddr)
	//   cidReg = LoadClassIdInstr(objReg)
	//   lsl  cidReg, cidReg, #1
	// branchAddr:
	auto& il = fnInfo->il_insns.back();
	if (insn.id() == ARM64_INS_LSL && insn.ops[0].reg == insn.ops[1].reg && insn.ops[2].imm == dart::kSmiTagSize && il->Kind() == ILInstr::LoadClassId && fnInfo->il_insns.size() >= 3) {
		auto il_loadClassId = reinterpret_cast<LoadClassIdInstr*>(il.get());
		if (il_loadClassId->cidReg == A64::FromCsReg(insn.ops[0].reg)) {
			auto& il2 = fnInfo->il_insns[fnInfo->il_insns.size() - 2];
			if (il2->Kind() == ILInstr::BranchIfSmi) {
				auto il_branchIfSmi = reinterpret_cast<BranchIfSmiInstr*>(il2.get());
				RELEASE_ASSERT(il_branchIfSmi->objReg == il_loadClassId->objReg);
				auto& il3 = fnInfo->il_insns[fnInfo->il_insns.size() - 3];
				RELEASE_ASSERT(il3->Kind() == ILInstr::LoadImm);
				auto il_loadImm = reinterpret_cast<LoadImmInstr*>(il3.get());
				RELEASE_ASSERT(il_loadImm->dstReg == il_loadClassId->cidReg);
				RELEASE_ASSERT(il_loadImm->val == dart::Smi::RawValue(dart::kSmiCid));

				// everything is OK, release all IL to cast to specific IL. Then, put them into a new IL
				il.release();
				il2.release();
				il3.release();
				auto il_new = new LoadTaggedClassIdMayBeSmiInstr(AddrRange(il_loadImm->Start(), insn.NextAddress()),
					std::unique_ptr<LoadImmInstr>(il_loadImm), std::unique_ptr<BranchIfSmiInstr>(il_branchIfSmi), 
					std::unique_ptr<LoadClassIdInstr>(il_loadClassId));
				fnInfo->il_insns.resize(fnInfo->il_insns.size() - 3);
				fnInfo->AddInstruction(std::unique_ptr<LoadTaggedClassIdMayBeSmiInstr>(il_new));
				return insn.ptr();
			}
		}
	}
	return nullptr;
}

static cs_insn* processBoxInt64Instr(AnalyzedFnData* fnInfo, AsmInstruction insn)
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
			auto stub = fnInfo->app.GetFunction(insn.ops[0].imm);
			ASSERT(stub->IsStub());
			const auto stubKind = reinterpret_cast<DartStub*>(stub)->kind;
			RELEASE_ASSERT(stubKind == DartStub::AllocateMintSharedWithoutFPURegsStub || stubKind == DartStub::AllocateMintSharedWithFPURegsStub);

			++insn;
			RELEASE_ASSERT(insn.id() == ARM64_INS_STUR);
			RELEASE_ASSERT(insn.ops[0].reg == in_reg);
			RELEASE_ASSERT(insn.ops[1].mem.base == out_reg && insn.ops[1].mem.disp == dart::Mint::value_offset() - dart::kHeapObjectTag);

			const auto nextAddr = insn.NextAddress();
			RELEASE_ASSERT(nextAddr == contAddr);

			const auto objReg = A64::FromCsReg(out_reg);
			const auto srcReg = A64::FromCsReg(in_reg);
			fnInfo->AddInstruction(std::make_unique<BoxInt64Instr>(AddrRange(insn0->address, nextAddr), objReg, srcReg));
			return insn.ptr();
		}
		else {
			--insn;
		}
	}
	return nullptr;
}

static cs_insn* processLoadInt32FromBoxOrSmiInstr(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	// From UnboxInstr::EmitLoadInt32FromBoxOrSmi(), includes branch if Smi
	//   output is raw integer, input is Dart object
	if (insn.id() == ARM64_INS_SBFX && insn.ops[2].imm == dart::kSmiTagSize && insn.ops[3].imm == 31) {
		auto insn0 = insn.ptr();
		const auto out_reg = insn.ops[0].reg;
		const auto in_reg = insn.ops[1].reg;
		const auto dstReg = A64::FromCsReg(out_reg);
		const auto srcReg = A64::FromCsReg(in_reg);
		// TODO: make srcReg type in current function register to be Smi

		++insn;
		if (insn.id() == ARM64_INS_TBZ && A64::FromCsReg(insn.ops[0].reg) == srcReg && insn.ops[1].imm == dart::kSmiTag) {
			// if not smi, get raw integer from Mint object
			// these instructions might not be here because compiler know the value is always be Smi
			auto cont_addr = insn.ops[2].imm;

			++insn;
			RELEASE_ASSERT(insn.id() == ARM64_INS_LDUR);
			RELEASE_ASSERT(insn.ops[0].reg == out_reg);
			RELEASE_ASSERT(insn.ops[1].mem.base == in_reg && insn.ops[1].mem.disp == dart::Mint::value_offset() - dart::kHeapObjectTag);

			// srcReg item type MUST be Mint

			RELEASE_ASSERT(insn.NextAddress() == cont_addr);
		}
		else {
			--insn;
			// srcReg object is always Smi
		}

		fnInfo->AddInstruction(std::make_unique<LoadInt32Instr>(AddrRange(insn0->address, insn.NextAddress()), dstReg, srcReg));
		return insn.ptr();
	}
	return nullptr;
}

static cs_insn* processTryAllocateObject(AnalyzedFnData* fnInfo, AsmInstruction insn)
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
		auto insn0_addr = insn.address();

		const auto inst_reg = insn.ops[0].reg;
		const auto temp_reg = insn.ops[1].reg;

		++insn;
		RELEASE_ASSERT(insn.id() == ARM64_INS_ADD);
		RELEASE_ASSERT(insn.ops[0].reg == inst_reg);
		RELEASE_ASSERT(insn.ops[1].reg == inst_reg);
		const auto inst_size = insn.ops[2].imm;
		RELEASE_ASSERT(inst_size == 0x10);

		++insn;
		RELEASE_ASSERT(insn.id() == ARM64_INS_CMP);
		RELEASE_ASSERT(insn.ops[0].reg == temp_reg);
		RELEASE_ASSERT(insn.ops[1].reg == inst_reg);

		++insn;
		RELEASE_ASSERT(insn.id() == ARM64_INS_B && insn.cc() == ARM64_CC_LS);
		const uint64_t slow_path = (uint64_t)insn.ops[0].imm;
		// slow path is always at the end of function
		RELEASE_ASSERT(insn.address() < slow_path && slow_path < fnInfo->dartFn.AddressEnd());

		++insn;
		if (insn.id() == ARM64_INS_NOP)
			++insn;

		RELEASE_ASSERT(insn.id() == ARM64_INS_STR);
		RELEASE_ASSERT(insn.ops[0].reg == inst_reg);
		RELEASE_ASSERT(insn.ops[1].mem.base == CSREG_DART_THR && insn.ops[1].mem.disp == dart::Thread::top_offset());

		++insn;
		RELEASE_ASSERT(insn.id() == ARM64_INS_SUB);
		RELEASE_ASSERT(insn.ops[0].reg == inst_reg);
		RELEASE_ASSERT(insn.ops[1].reg == inst_reg);
		RELEASE_ASSERT(insn.ops[2].imm == inst_size - 1); // -1 because of kHeapObjectTag

		// also can reuse loadImm
		++insn;
		RELEASE_ASSERT(insn.IsMovz());
		RELEASE_ASSERT(insn.ops[0].reg == temp_reg);
		auto tag = insn.ops[1].imm;

		++insn;
		// movk instruction is needed with bits 16-32 is not zero (class id > 16)
		if (insn.id() == ARM64_INS_MOVK) {
			RELEASE_ASSERT(insn.ops[0].reg == temp_reg);
			RELEASE_ASSERT(insn.ops[1].shift.value == 16);
			tag |= (insn.ops[1].imm << 16);
			++insn;
		}

		RELEASE_ASSERT(insn.id() == ARM64_INS_STUR);
		RELEASE_ASSERT(insn.ops[0].reg == temp_reg);
		RELEASE_ASSERT(insn.ops[1].mem.base == inst_reg && insn.ops[1].mem.disp == -1); // because of kHeapObjectTag

		const uint32_t cid = (tag >> 12) & 0xfffff;
		auto dartCls = fnInfo->app.GetClass(cid);
		//RELEASE_ASSERT(dartCls->Size() < 0 || dartCls->Size() == inst_size);

		const auto dstReg = A64::FromCsReg(inst_reg);
		fnInfo->AddInstruction(std::make_unique<AllocateObjectInstr>(AddrRange(insn0_addr, insn.NextAddress()), dstReg, *dartCls));

		// TODO: extra check by walk through slow path

		return insn.ptr();
	}

	return nullptr;
}

static std::unique_ptr<WriteBarrierInstr> processWriteBarrier(AnalyzedFnData* fnInfo, AsmInstruction insn, cs_insn** last_ins)
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
	auto ins0_addr = insn.address();
	//const auto objReg = A64::FromDartReg(dart::kWriteBarrierObjectReg);
	//const auto valReg = A64::FromDartReg(dart::kWriteBarrierValueReg);
	A64::Register objReg = A64::kNoRegister;
	A64::Register valReg = A64::kNoRegister;
	uint64_t contSmiAddr = 0;
	if (insn.id() == ARM64_INS_TBZ) {
		if (insn.ops[1].imm != dart::kSmiTag)
			return nullptr;
		valReg = A64::FromCsReg(insn.ops[0].reg);
		contSmiAddr = (uint64_t)insn.ops[2].imm;
		++insn;
		if (insn.id() == ARM64_INS_NOP)
			++insn;
	}

	if (insn.id() != ARM64_INS_LDURB || A64::FromCsReg(insn.ops[0].reg) != A64::TMP_REG || insn.ops[1].mem.disp != -1)
		return nullptr; // it is not WriteBarrier
	objReg = A64::FromCsReg(insn.ops[1].mem.base);

	++insn;
	if (insn.id() != ARM64_INS_LDURB || A64::FromCsReg(insn.ops[0].reg) != A64::TMP2_REG || insn.ops[1].mem.disp != -1)
		return nullptr;
	if (valReg != A64::kNoRegister) {
		RELEASE_ASSERT(A64::FromCsReg(insn.ops[1].mem.base) == valReg);
	}
	else {
		valReg = A64::FromCsReg(insn.ops[1].mem.base);
	}

	++insn;
	RELEASE_ASSERT(insn.id() == ARM64_INS_AND);
	RELEASE_ASSERT(A64::FromCsReg(insn.ops[0].reg) == A64::TMP_REG);
	RELEASE_ASSERT(A64::FromCsReg(insn.ops[1].reg) == A64::TMP2_REG);
	RELEASE_ASSERT(A64::FromCsReg(insn.ops[2].reg) == A64::TMP_REG);
	RELEASE_ASSERT(insn.ops[2].shift.type == ARM64_SFT_LSR && insn.ops[2].shift.value == 2);

	++insn;
	RELEASE_ASSERT(insn.id() == ARM64_INS_TST);
	RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_TMP);
	RELEASE_ASSERT(insn.ops[1].reg == CSREG_DART_HEAP);
	RELEASE_ASSERT(insn.ops[1].shift.type == ARM64_SFT_LSR && insn.ops[1].shift.value == 32);

	++insn;
	RELEASE_ASSERT(insn.id() == ARM64_INS_B && insn.cc() == ARM64_CC_EQ);
	const auto contAddr = (uint64_t)insn.ops[0].imm;
	RELEASE_ASSERT(contSmiAddr == 0 || contSmiAddr == contAddr);

	++insn;
	if (insn.id() == ARM64_INS_NOP)
		++insn;
	bool spill_lr = false;
	if (insn.id() == ARM64_INS_STR) {
		// spill_lr => Push(LR)
		// 0x1d5f58: str  x30, [x15, #-8]!
		RELEASE_ASSERT(insn.writeback());
		RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
		RELEASE_ASSERT(insn.ops[1].mem.base == CSREG_DART_SP && insn.ops[1].mem.disp == -8);
		spill_lr = true;
		++insn;
	}

	// TODO: call instruction is not processed. AsmText cannot show the stub name.
	RELEASE_ASSERT(insn.id() == ARM64_INS_BL);
	auto stub = fnInfo->app.GetFunction(insn.ops[0].imm);
	RELEASE_ASSERT(stub->IsStub());
	const auto stubKind = reinterpret_cast<DartStub*>(stub)->kind;
	RELEASE_ASSERT(stubKind == DartStub::WriteBarrierWrappersStub || stubKind == DartStub::ArrayWriteBarrierStub);
	bool isArray = stubKind == DartStub::ArrayWriteBarrierStub;

	if (spill_lr) {
		++insn;
		// 0x1d5f60: ldr  x30, [x15], #8
		RELEASE_ASSERT(insn.id() == ARM64_INS_LDR && insn.writeback());
		RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_LR);
		RELEASE_ASSERT(insn.ops[1].mem.base == CSREG_DART_SP);
		RELEASE_ASSERT(insn.ops[2].imm == 8);
	}

	RELEASE_ASSERT(insn.NextAddress() == contAddr);

	*last_ins = insn.ptr();

	return std::make_unique<WriteBarrierInstr>(AddrRange(ins0_addr, insn.NextAddress()), objReg, valReg, isArray);
}

static cs_insn* processWriteBarrier(AnalyzedFnData* fnInfo, AsmInstruction insn)
{
	// 0x2a7684: bl  #0x44bdf0  # ArrayWriteBarrierStub
	//const auto objReg = A64::FromDartReg(dart::kWriteBarrierObjectReg);
	//const auto valReg = A64::FromDartReg(dart::kWriteBarrierValueReg);
	cs_insn* last_ins = nullptr;
	auto instr = processWriteBarrier(fnInfo, insn, &last_ins);
	if (instr) {
		fnInfo->AddInstruction(std::move(instr));
		return last_ins;
	}

	return nullptr;
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

static cs_insn* processLoadStore(AnalyzedFnData* fnInfo, AsmInstruction insn)
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
				return nullptr; // not array load/store

			const auto objReg = A64::FromDartReg(dart::kWriteBarrierObjectReg);
			const auto valReg = A64::FromDartReg(dart::kWriteBarrierValueReg);
			// TODO: objReg type MUST be Dart array
			auto idx = VarStorage::NewSmallImm(0);
			if (insn.ops[2].type == ARM64_OP_IMM) {
				// const int64_t offset = index * index_scale + HeapDataOffset(is_external, cid);
				const auto arr_idx = (insn.ops[2].imm + dart::kHeapObjectTag - dart::Array::data_offset()) / dart::kCompressedWordSize;
				if (arr_idx < 0)
					return nullptr;
				idx = VarStorage::NewSmallImm(arr_idx);
			}
			else {
				// register as index
				RELEASE_ASSERT(insn.ops[2].shift.type == ARM64_SFT_LSL && 
					(insn.ops[2].shift.value == dart::kCompressedWordSizeLog2 || 
						(insn.ops[2].shift.value == dart::kCompressedWordSizeLog2 - 1 || insn.ops[2].ext == ARM64_EXT_SXTW)));
				idx = VarStorage(A64::FromCsReg(insn.ops[2].reg));

				++insn;
				RELEASE_ASSERT(insn.id() == ARM64_INS_ADD);
				RELEASE_ASSERT(insn.ops[0].reg == CSREG_DART_WB_SLOT);
				RELEASE_ASSERT(insn.ops[1].reg == CSREG_DART_WB_SLOT);
				RELEASE_ASSERT(insn.ops[2].imm == dart::Array::data_offset() - dart::kHeapObjectTag);
			}

			++insn;
			RELEASE_ASSERT(insn.id() == ARM64_INS_STR);
			RELEASE_ASSERT(A64::FromCsReg(insn.ops[0].reg) == valReg && GetCsRegSize(insn.ops[0].reg) == dart::kCompressedWordSize);
			RELEASE_ASSERT(insn.ops[1].mem.base == CSREG_DART_WB_SLOT && insn.ops[1].mem.disp == 0);

			++insn;
			cs_insn* last_ins = nullptr;
			auto instr = processWriteBarrier(fnInfo, insn, &last_ins);
			RELEASE_ASSERT(instr);
			RELEASE_ASSERT(instr->isArray);
			RELEASE_ASSERT(instr->objReg == objReg && instr->valReg == valReg);

			ArrayOp arrayOp(dart::kCompressedWordSize, false, ArrayOp::List);
			fnInfo->AddInstruction(std::make_unique<StoreArrayElementInstr>(AddrRange(ins0_addr, last_ins->address + last_ins->size), valReg, objReg, idx, arrayOp));
			return last_ins;
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
			const auto arrReg = A64::FromCsReg(insn.ops[1].reg);
			auto idx = VarStorage(A64::FromCsReg(insn.ops[2].reg));
			const auto shift = insn.ops[2].shift;
			if (shift.type != ARM64_SFT_INVALID && shift.type != ARM64_SFT_LSL)
				return nullptr;
			const auto ext = insn.ops[2].ext;

			++insn;
			// array offset for index as register always be start of payload
			const auto arr_data_offset = insn.ops[1].mem.disp;
			if (insn.ops[1].mem.base != tmpReg || arr_data_offset < 8)
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
				//	RELEASE_ASSERT(ext == ARM64_EXT_SXTW);
				// TODO: this index is Smi
			}
			else {
				FATAL("invalid shift for array operation");
			}
			bool isTypedData = dart::UntaggedTypedData::payload_offset() - dart::kHeapObjectTag == arr_data_offset;
			RELEASE_ASSERT(isTypedData || arr_data_offset == dart::Array::data_offset() - dart::kHeapObjectTag);
			if (arrayOp.isLoad) {
				// load always uses TMP register
				RELEASE_ASSERT(tmpReg == CSREG_DART_TMP);
				const auto dstReg = A64::FromCsReg(insn.ops[0].reg);
				auto instr = std::make_unique<LoadArrayElementInstr>(AddrRange(ins0_addr, insn.NextAddress()), dstReg, arrReg, idx, arrayOp);
				fnInfo->AddInstruction(std::move(instr));
			}
			else {
				// store operation
				const auto valReg = A64::FromCsReg(insn.ops[0].reg);
				auto instr = std::make_unique<StoreArrayElementInstr>(AddrRange(ins0_addr, insn.NextAddress()), valReg, arrReg, idx, arrayOp);
				fnInfo->AddInstruction(std::move(instr));
			}
			return insn.ptr();
		}
	}

	if (insn.ops[1].mem.base != CSREG_DART_FP && insn.ops[1].mem.disp != 0) {
		// load/store with fixed offset
		const auto arrayOp = getArrayOp(insn);
		if (arrayOp.IsArrayOp()) {
			cs_insn* last_ins = nullptr;
			const auto valReg = A64::FromCsReg(insn.ops[0].reg);
			const auto objReg = A64::FromCsReg(insn.ops[1].mem.base);
			const auto offset = insn.ops[1].mem.disp;
			if (arrayOp.arrType == ArrayOp::Unknown) {
				// might be array or object. set it as object first.
				if (arrayOp.isLoad) {
					fnInfo->AddInstruction(std::make_unique<LoadFieldInstr>(insn.ptr(), valReg, objReg, offset));
					last_ins = insn.ptr();
				}
				else {
					//
					auto insn2 = insn.Next();
					auto instr = processWriteBarrier(fnInfo, insn2, &last_ins);
					if (instr) {
						// with WriteBearrier, we can determine if the object is array or not
						// but it is still difficult if it is list or typed data
						RELEASE_ASSERT(instr->objReg == objReg && instr->valReg == valReg);
						if (instr->isArray)
							auto instr = std::make_unique<StoreArrayElementInstr>(AddrRange(insn.address(), last_ins->address + last_ins->size), valReg, objReg, VarStorage::NewSmallImm(offset), arrayOp);
						else
							fnInfo->AddInstruction(std::make_unique<StoreFieldInstr>(AddrRange(insn.address(), last_ins->address + last_ins->size), valReg, objReg, offset));
					}
					else {
						fnInfo->AddInstruction(std::make_unique<StoreFieldInstr>(insn.ptr(), valReg, objReg, offset));
						last_ins = insn.ptr();
					}
				}
			}
			else {
				// array
				auto idx = VarStorage::NewSmallImm((offset + dart::kHeapObjectTag - dart::UntaggedTypedData::payload_offset()) / arrayOp.size);
				if (arrayOp.isLoad) {
					fnInfo->AddInstruction(std::make_unique<LoadArrayElementInstr>(AddrRange(insn.address(), insn.NextAddress()), valReg, objReg, idx, arrayOp));
					last_ins = insn.ptr();
				}
				else {
					auto insn2 = insn.Next();
					auto instr = processWriteBarrier(fnInfo, insn2, &last_ins);
					if (instr) {
						fnInfo->AddInstruction(std::make_unique<StoreArrayElementInstr>(AddrRange(insn.address(), last_ins->address + last_ins->size), valReg, objReg, idx, arrayOp));
					}
					else {
						fnInfo->AddInstruction(std::make_unique<StoreArrayElementInstr>(AddrRange(insn.address(), insn.NextAddress()), valReg, objReg, idx, arrayOp));
						last_ins = insn.ptr();
					}
				}
			}

			return last_ins;
		}
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
	processLoadImmInstr,
	processBranchIfSmiInstr,
	processLoadClassIdInstr,
	processBoxInt64Instr,
	processLoadInt32FromBoxOrSmiInstr,
	processLoadTaggedClassIdMayBeSmiInstr,
	processLoadFieldTableInstr,
	processTryAllocateObject,
	processWriteBarrier,
	processLoadStore,
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
				auto il = fnInfo->il_insns.back().get();
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

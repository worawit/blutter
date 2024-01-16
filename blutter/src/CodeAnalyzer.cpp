#include "pch.h"
#include "CodeAnalyzer.h"
#include "DartApp.h"

#ifndef NO_CODE_ANALYSIS

AnalyzedFnData::AnalyzedFnData(DartApp& app, DartFunction& dartFn, AsmTexts asmTexts)
	: app(app), dartFn(dartFn), asmTexts(std::move(asmTexts))
{
}

void CodeAnalyzer::AnalyzeAll()
{
	Disassembler disasmer;

	for (auto lib : app.libs) {
		if (lib->isInternal)
			continue;
		for (auto cls : lib->classes) {
			for (auto dartFn : cls->Functions()) {
				if (dartFn->Size() == 0)
					continue;

				// start from PayloadAddress or Address?
				// the assemblies will be deleted after finish analysis because assembly with details consume too much memory
				auto asm_insns = disasmer.Disasm((uint8_t*)dartFn->MemAddress(), dartFn->Size(), dartFn->Address());

				dartFn->SetAnalyzedData(std::make_unique<AnalyzedFnData>(app, *dartFn, convertAsm(asm_insns)));

				asm2il(dartFn, asm_insns);
			}
		}
	}
}

#endif // NO_CODE_ANALYSIS

std::string FnParamInfo::ToString() const
{
	std::string txt = type ? type->ToString() : "dynamic";
	txt += ' ';
	txt += name.empty() ? "_" : name;
	if (val) {
		txt += " = ";
		txt += val->ToString();
	}
	if (valReg.IsSet() || localOffset) {
		txt += " /* ";
		if (valReg.IsSet()) {
			txt += valReg.Name();
			if (localOffset)
				txt += ", ";
		}
		if (localOffset)
			txt += std::format("fp-{:#x}", -localOffset);
		txt += " */";
	}
	return txt;
}

FnParamInfo* FnParams::findValReg(A64::Register reg)
{
	for (auto& param : params) {
		if (param.valReg == reg) {
			return &param;
		}
	}
	return nullptr;
}

bool FnParams::movValReg(A64::Register dstReg, A64::Register srcReg)
{
	for (auto& param : params) {
		if (param.valReg == srcReg) {
			param.valReg = dstReg;
			return true;
		}
	}
	return false;
}

std::string FnParams::ToString() const
{
	std::string txt;
	for (int i = 0; i < params.size(); i++) {
		if (i != 0)
			txt += ", ";
		if (i == numFixedParam)
			txt += isNamedParam ? '{' : '[';

		txt += params[i].ToString();
	}

	if (numFixedParam < params.size())
		txt += isNamedParam ? '}' : ']';

	return txt;
}

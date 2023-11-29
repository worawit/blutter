#include "pch.h"
#include "CodeAnalyzer.h"
#include "DartApp.h"

AnalyzedFnData::AnalyzedFnData(DartApp& app, DartFunction& dartFn, AsmTexts asmTexts)
	: app(app), dartFn(dartFn), asmTexts(std::move(asmTexts)), last_ret(nullptr), stackSize(0), useFramePointer(false)
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

std::string FnParamInfo::ToString() const
{
	std::string txt = type ? type->ToString() : "dynamic";
	txt += ' ';
	txt += name.empty() ? "_" : name;
	if (val) {
		txt += " = ";
		txt += val->ToString();
	}
	if (valReg.IsSet()) {
		txt += " /* ";
		txt += valReg.Name();
		if (localOffset)
			txt += std::format(", fp-{:#x}", -localOffset);
		txt += " */";
	}
	return txt;
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
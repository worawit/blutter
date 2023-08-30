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

#include "pch.h"
#include "il.h"
#include "CodeAnalyzer.h"

std::string SetupParametersInstr::ToString()
{
	return "SetupParameters(" + params->ToString() + ")";
}
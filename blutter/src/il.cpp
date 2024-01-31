#include "pch.h"
#include "il.h"
#include "CodeAnalyzer.h"
#include "DartThreadInfo.h"

std::string SetupParametersInstr::ToString()
{
	return "SetupParameters(" + params->ToString() + ")";
}

std::string CallLeafRuntimeInstr::ToString()
{
	const auto& name = GetThreadOffsetName(thrOffset);
	const auto info = GetThreadLeafFunction(thrOffset);
	return std::format("CallRuntime_{}({}) -> {}", name, info->params, info->returnType);
}
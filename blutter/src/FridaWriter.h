#pragma once
#include "DartApp.h"

class FridaWriter
{
public:
	FridaWriter(DartApp& app) : app(app) {};

	void Create(const char* filename);

private:
	DartApp& app;
};


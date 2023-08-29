#include "pch.h"
#include "DartApp.h"
#include "DartDumper.h"
#include "CodeAnalyzer.h"
#include "FridaWriter.h"
#include "args.hxx"
#include <filesystem>

int main(int argc, char** argv)
{
	args::ArgumentParser parser("B(l)utter - Reversing flutter application", "");
	args::HelpFlag help(parser, "help", "Display this help menu", { 'h', "help" });
	args::Group reqGrp(parser, "Required arguments", args::Group::Validators::All);
	args::ValueFlag<std::string> infile(reqGrp, "infile", "libapp file", { 'i', "in" });
	args::ValueFlag<std::string> outdir(reqGrp, "outdir", "out path", { 'o', "out"});

	try {
		parser.ParseCLI(argc, argv);

		auto& libappPath = args::get(infile);

		std::filesystem::path outDir(args::get(outdir));
		std::filesystem::create_directory(outDir);

		DartApp app(libappPath.c_str());
		std::cout << std::format("libapp is loaded at {:#x}\n", app.base());
		std::cout << std::format("Dart heap at {:#x}\n", app.heap_base());

		app.EnterScope();
		app.LoadInfo();
		app.ExitScope();

		app.EnterScope();
		std::cout << "Analyzing the application\n";
		CodeAnalyzer analyzer(app);
		analyzer.AnalyzeAll();

		DartDumper dumper(app);
		std::cout << "Dumping Object Pool\n";
		dumper.DumpObjectPool((outDir / "pp.txt").string().c_str());
		dumper.DumpObjects((outDir / "objs.txt").string().c_str());
		std::cout << "Generating application assemblies\n";
		dumper.DumpCode((outDir / "asm").string().c_str());
		dumper.Dump4Ida(outDir / "ida_script");
		
		std::cout << "Generating Frida script\n";
		FridaWriter fwriter(app);
		fwriter.Create((outDir / "blutter_frida.js").string().c_str());

		app.ExitScope();
	}
	catch (args::Help&) {
		std::cout << parser;
		return 0;
	}
	catch (args::ParseError& e) {
		std::cerr << e.what() << std::endl;
		std::cerr << parser;
		return 1;
	}
	catch (args::ValidationError& e) {
		std::cerr << e.what() << std::endl;
		std::cerr << parser;
		return 1;
	}
	catch (std::exception& e) {
		printf("exception: %s", e.what());
	}

	return 0;
}
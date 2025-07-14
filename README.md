# B(l)utter
Flutter Mobile Application Reverse Engineering Tool by Compiling Dart AOT Runtime

It provides data on flutter including:
- pseudo assembly code broken out by class/function with named calls
- ida script to add class/function and member field meta data
- frida script to allow live tracing function calls on unmodified binaries


Currently the application supports only Android libapp.so (arm64 only).
Also the application is currently work only against recent Dart versions.

For high priority missing features, see [TODO](#todo)

<!-- MarkdownTOC -->

- [Environment Setup](#environment-setup)
	- [Debian Unstable \(gcc 13\)](#debian-unstable-gcc-13)
	- [Windows](#windows)
	- [macOS Ventura and Sonoma \(clang 16\)](#macos-ventura-and-sonoma-clang-16)
- [Usage](#usage)
- [Update](#update)
- [Output files](#output-files)
- [Directories](#directories)
- [Frida Trace Script](#frida-trace-script)
- [Troubleshooting](#troubleshooting)
- [Development](#development)
	- [Generating Visual Studio Solution](#generating-visual-studio-solution)
	- [TODO](#todo)

<!-- /MarkdownTOC -->


## Environment Setup
This application uses C++20 Formatting library. It requires very recent C++ compiler such as g++>=13, Clang>=16 or MSVC 17.

I recommend using Linux OS (only tested on Deiban sid/trixie) because it is easy to setup.

### Debian Unstable (gcc 13)
**_NOTE:_**
Use ONLY Debian/Ubuntu version that provides gcc>=13 from its own main repository.
Using ported gcc to old Debian/Ubuntu version does not work.

- Install build tools and depenencies
```
apt install python3-pyelftools python3-requests git cmake ninja-build \
    build-essential pkg-config libicu-dev libcapstone-dev
```

### Windows
- Install git and python 3
- Install latest Visual Studio with "Desktop development with C++" and "C++ CMake tools"
- Run init script to install required libraries (libcapstone and libicu4c):
```
python scripts\init_env_win.py
```
- Install python elf tools `pip install pyelftools`
- Start "x64 Native Tools Command Prompt"

### macOS Ventura and Sonoma (clang 16)
- Install XCode
- Install clang 16 and required tools
```
brew install llvm@16 cmake ninja pkg-config icu4c capstone
pip3 install pyelftools requests
```

## Usage
Extract "lib" directory from apk file
```
python3 blutter.py path/to/app/lib/arm64-v8a out_dir
```
The blutter.py will automatically detect the Dart version from the flutter engine and call executable of blutter to get the information from libapp.so.

If the blutter executable for required Dart version does not exists, the script will automatically checkout Dart source code and compiling it.

## Update
You can use ```git pull``` to update and run blutter.py with ```--rebuild``` option to force rebuild the executable
```
python3 blutter.py path/to/app/lib/arm64-v8a out_dir --rebuild
```

## Output files
- **asm/\*** libapp assemblies with symbols
- **blutter_frida.js** the frida script template for the target application
- **objs.txt** complete (nested) dump of Object from Object Pool
- **pp.txt** all Dart objects in Object Pool


## Directories
- **bin** contains blutter executables for each Dart version in "blutter_dartvm\<ver\>\_\<os\>\_\<arch\>" format
- **blutter** contains source code. need building against Dart VM library
- **build** contains building projects which can be deleted after finishing the build process
- **dartsdk** contains checkout of Dart Runtime which can be deleted after finishing the build process
- **external** contains 3rd party libraries for Windows only
- **packages** contains the static libraries of Dart Runtime
- **scripts** contains python scripts for getting/building Dart

## Frida Trace Script
The Frida trace script (found as `output_dir/blutter_frida.js`) allows you to spy on function calls to flutter functions and gives you information about the args/vals passed to it.  It tries to recursively display the entire parameter structure and vals but it makes some educated guesses that can be wrong.  This can result in crashes. It will print the function it is about to try and trace when it goes to trace so if it does crash try to exclude that function from being spied.

To use this you need to have frida working on the device either the frida server or the frida gadget injected into the package.  Instructions for that are beyond the scope of this project, essentially normal frida-trace / frida / objection commands should work.

Then to use this edit the BlutterOpts initialization to include the options you want. There are 3 primary functions to call to add traces (they can be called as many times as you want):
    
- `opts.IgnoreByClassFuncRegex(regex : RegExp)`
- `opts.SpyByClassFuncRegex(regex : RegExp, maxDepth : number = MaxDepth)`
- `opts.SpyByFunctionAddy(address : number, displayName : string = "", maxDepth : number = MaxDepth)`

An optional config would then look like:
```javascript
function GetOptions(){
  opts.IgnoreByClassFuncRegex(/(anim|battery|anon_|build|widget|Dependencies|Observer|Render)/i);
  opts.SpyByClassFuncRegex(/interestingclass.+::func_prefix.+/i,4);
  opts.SpyByClassFuncRegex(/moreInterestingClassByCrashProne.+::myFunc.+/,0);
  opts.SpyByFunctionAddy(0x2fd950, "ImportantFunc",10);
  return opts;
}
```

NOTE: Any changes to the script are overwritten when you regenerate/update the output
  
Finally launch it like any other frida script ie:
  
  `frida -U -l blutter_frida.py "AppToSpy"`

To find function name look at the output_dir/ida_script/addNames.py file it lists all the functions at the top.

## Troubleshooting
If you get errors during compiling / initial run make sure you pay attention to the Environment setup in detail, have the deps installed, and for example on Windows are running the script from the Developer Powershell VS instance otherwise you won't have the required items in your path.

## Development

### Generating Visual Studio Solution
I use Visual Studio to delevlop Blutter on Windows. ```--vs-sln``` options can be used to generate a Visual Studio solution.
```
python blutter.py path\to\lib\arm64-v8a build\vs --vs-sln
```

### TODO
- More code analysis
  - Function arguments and return type
  - Some psuedo code for code pattern
- Generate better Frida script
  - More internal classes
  - Object modification
- Obfuscated app (still missing many functions)
- Reading iOS binary
- Input as apk or ipa

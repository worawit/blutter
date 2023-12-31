#!/usr/bin/python3
import argparse
import glob
import mmap
import os
import platform
import shutil
import subprocess
import sys

CMAKE_CMD = "cmake"
NINJA_CMD = "ninja"

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
BIN_DIR = os.path.join(SCRIPT_DIR, 'bin')
PKG_INC_DIR = os.path.join(SCRIPT_DIR, 'packages', 'include')
PKG_LIB_DIR = os.path.join(SCRIPT_DIR, 'packages', 'lib')
BUILD_DIR = os.path.join(SCRIPT_DIR, 'build')

def find_lib_files(indir: str):
    app_file = os.path.join(indir, 'libapp.so')
    if not os.path.isfile(app_file):
        app_file = os.path.join(indir, 'App')
        if not os.path.isfile(app_file):
            sys.exit("Cannot find libapp file")
    
    flutter_file = os.path.join(indir, 'libflutter.so')
    if not os.path.isfile(flutter_file):
        flutter_file = os.path.join(indir, 'Flutter')
        if not os.path.isfile(flutter_file):
            sys.exit("Cannot find libflutter file")
    
    return os.path.abspath(app_file), os.path.abspath(flutter_file)

def find_compat_macro(dart_version: str):
    macros = []
    include_path = os.path.join(PKG_INC_DIR, f'dartvm{dart_version}')
    vm_path = os.path.join(include_path, 'vm')
    with open(os.path.join(vm_path, 'class_id.h'), 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access = mmap.ACCESS_READ)
        # Rename the default implementation classes of Map and Set https://github.com/dart-lang/sdk/commit/a2de36e708b8a8e15d3bd49eef2cede57e649436
        if mm.find(b'ImmutableLinkedHashMap') != -1:
            macros.append('-DOLD_MAP_SET_NAME=1')
        # Remove TypeRef https://github.com/dart-lang/sdk/commit/2ee6fcf5148c34906c04c2ac518077c23891cd1b
        # in this commit also added RecordType as sub class of AbstractType
        #   so assume Dart Records implementation is completed in this commit (before this commit is inconpmlete RecordType)
        if mm.find(b'V(TypeRef)') != -1:
            macros.append('-DHAS_TYPE_REF=1')
        else:
            assert(mm.find(b'V(RecordType)') != -1)
            macros.append('-DHAS_RECORD_TYPE=1')
    
    with open(os.path.join(vm_path, 'class_table.h'), 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access = mmap.ACCESS_READ)
        # Clean up ClassTable (Merge ClassTable and SharedClassTable back together)
        # https://github.com/dart-lang/sdk/commit/4a4eedd860a8af2b1cb27e68d9feae5550d0f511
        # the commit moved GetUnboxedFieldsMapAt() from SharedClassTable to ClassTable
        if mm.find(b'class SharedClassTable {') != -1:
            macros.append('-DHAS_SHARED_CLASS_TABLE=1')
    
    return macros

def cmake_blutter(blutter_name: str, dartlib_name: str, macros: list):
    blutter_dir = os.path.join(SCRIPT_DIR, 'blutter')
    builddir = os.path.join(BUILD_DIR, blutter_name)
        
    my_env = None
    if platform.system() == 'Darwin':
        llvm_path = subprocess.run(['brew', '--prefix', 'llvm@15'], capture_output=True, check=True).stdout.decode().strip()
        clang_file = os.path.join(llvm_path, 'bin', 'clang')
        my_env = {**os.environ, 'CC': clang_file, 'CXX': clang_file+'++'}
    # cmake -GNinja -Bbuild -DCMAKE_BUILD_TYPE=Release
    subprocess.run([CMAKE_CMD, '-GNinja', '-B', builddir, f'-DDARTLIB={dartlib_name}', '-DCMAKE_BUILD_TYPE=Release', '--log-level=NOTICE'] + macros, cwd=blutter_dir, check=True, env=my_env)

    # build and install blutter
    subprocess.run([NINJA_CMD], cwd=builddir, check=True)
    subprocess.run([CMAKE_CMD, '--install', '.'], cwd=builddir, check=True)

def main(indir: str, outdir: str, rebuild_blutter: bool, create_vs_sln: bool):
    libapp_file, libflutter_file = find_lib_files(indir)

    # getting dart version
    from extract_dart_info import extract_dart_info
    dart_version, snapshot_hash, arch, os_name = extract_dart_info(libapp_file, libflutter_file)
    print(f'Dart version: {dart_version}, Snapshot: {snapshot_hash}, Target: {os_name} {arch}')

    # get the blutter executable filename
    from dartvm_fetch_build import fetch_and_build, get_dartlib_name
    dartlib_name = get_dartlib_name(dart_version, arch, os_name)
    blutter_name = f'blutter_{dartlib_name}'
    blutter_file = os.path.join(BIN_DIR, blutter_name) + ('.exe' if os.name == 'nt' else '')

    if not os.path.isfile(blutter_file):
        # before fetch and build, check the existence of compiled library first
        #   so the src and build directories can be deleted
        if os.name == 'nt':
            dartlib_file = os.path.join(PKG_LIB_DIR, dartlib_name+'.lib')
        else:
            dartlib_file = os.path.join(PKG_LIB_DIR, 'lib'+dartlib_name+'.a')
        if not os.path.isfile(dartlib_file):
            fetch_and_build(dart_version, arch, os_name)
        
        rebuild_blutter = True

    # creating Visual Studio solution overrides building
    if create_vs_sln:
        macros = find_compat_macro(dart_version)
        blutter_dir = os.path.join(SCRIPT_DIR, 'blutter')
        dbg_output_path = os.path.abspath(os.path.join(outdir, 'out'))
        dbg_cmd_args = f'-i {libapp_file} -o {dbg_output_path}'
        subprocess.run([CMAKE_CMD, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-B', outdir, f'-DDARTLIB={dartlib_name}', f'-DDBG_CMD:STRING={dbg_cmd_args}'] + macros + [blutter_dir], check=True)
        dbg_exe_dir = os.path.join(outdir, 'Debug')
        os.makedirs(dbg_exe_dir, exist_ok=True)
        for filename in glob.glob(os.path.join(BIN_DIR, '*.dll')):
            shutil.copy(filename, dbg_exe_dir)
    else:
        if rebuild_blutter:
            # do not use SDK path for checking source code because Blutter does not depended on it and SDK might be removed
            macros = find_compat_macro(dart_version)
            cmake_blutter(blutter_name, dartlib_name, macros)
            assert os.path.isfile(blutter_file), "Build complete but cannot find Blutter binary: " + blutter_file

        # execute blutter    
        subprocess.run([blutter_file, '-i', libapp_file, '-o', outdir])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        prog='B(l)utter',
        description='Reversing a flutter application tool')
    # TODO: accept apk or ipa
    parser.add_argument('indir', help='A directory directory that contains both libapp.so and libflutter.so')
    parser.add_argument('outdir', help='An output directory')
    parser.add_argument('--rebuild', action='store_true', default=False, help='Force rebuild the Blutter executable')
    parser.add_argument('--vs-sln', action='store_true', default=False, help='Generate Visual Studio solution at <outdir>')
    args = parser.parse_args()

    main(args.indir, args.outdir, args.rebuild, args.vs_sln)

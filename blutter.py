#!/usr/bin/python3
import argparse
import os
import platform
import subprocess
import sys

CMAKE_CMD = "cmake"
NINJA_CMD = "ninja"

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
BIN_DIR = os.path.join(SCRIPT_DIR, 'bin')
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
    
    return app_file, flutter_file

def cmake_blutter(blutter_name: str, dartlib_name: str):
    blutter_dir = os.path.join(SCRIPT_DIR, 'blutter')
    builddir = os.path.join(BUILD_DIR, blutter_name)
        
    my_env = None
    if platform.system() == 'Darwin':
        llvm_path = subprocess.run(['brew', '--prefix', 'llvm@15'], capture_output=True, check=True).stdout.decode().strip()
        clang_file = os.path.join(llvm_path, 'bin', 'clang')
        my_env = {**os.environ, 'CC': clang_file, 'CXX': clang_file+'++'}
    # cmake -GNinja -Bbuild -DCMAKE_BUILD_TYPE=Release
    subprocess.run([CMAKE_CMD, '-GNinja', '-B', builddir, f'-DDARTLIB={dartlib_name}', '-DCMAKE_BUILD_TYPE=Release', '--log-level=NOTICE'], cwd=blutter_dir, check=True, env=my_env)

    # build and install blutter
    subprocess.run([NINJA_CMD], cwd=builddir, check=True)
    subprocess.run([CMAKE_CMD, '--install', '.'], cwd=builddir, check=True)

def main(indir: str, outdir: str, rebuild_blutter: bool):
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

    if rebuild_blutter:
        cmake_blutter(blutter_name, dartlib_name)
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
    args = parser.parse_args()

    main(args.indir, args.outdir, args.rebuild)

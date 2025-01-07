import os
import shutil
import stat
import subprocess
import sys

# assume git and cmake (64 bits) command is in PATH
GIT_CMD = "git"
CMAKE_CMD = "cmake"
NINJA_CMD = "ninja"

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
CMAKE_TEMPLATE_FILE = os.path.join(SCRIPT_DIR, 'scripts', 'CMakeLists.txt')
CREATE_SRCLIST_FILE = os.path.join(SCRIPT_DIR, 'scripts', 'dartvm_create_srclist.py')
MAKE_VERSION_FILE = os.path.join(SCRIPT_DIR, 'scripts', 'dartvm_make_version.py')

SDK_DIR = os.path.join(SCRIPT_DIR, 'dartsdk')
BUILD_DIR = os.path.join(SCRIPT_DIR, 'build')

#DART_GIT_URL = 'https://dart.googlesource.com/sdk.git'
DART_GIT_URL = 'https://github.com/dart-lang/sdk.git'

imp_replace_snippet = """import importlib.util
import importlib.machinery

def load_source(modname, filename):
    loader = importlib.machinery.SourceFileLoader(modname, filename)
    spec = importlib.util.spec_from_file_location(modname, filename, loader=loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module
"""

class DartLibInfo:
    def __init__(self, version: str, os_name: str, arch: str, has_compressed_ptrs: bool | None = None, snapshot_hash: str | None = None):
        self.version = version
        self.os_name = os_name
        self.arch = arch
        self.snapshot_hash = snapshot_hash
        if has_compressed_ptrs is None:
            # use same as flutter default configuration
            # TODO: old Dart version has no pointer compression
            self.has_compressed_ptrs = os_name != 'ios'
        else:
            self.has_compressed_ptrs = has_compressed_ptrs
        self.lib_name = f'dartvm{version}_{os_name}_{arch}'


def checkout_dart(info: DartLibInfo):
    clonedir = os.path.join(SDK_DIR, 'v'+info.version)

    # if no version file,assume previous clone is failed. delete the whole directory and try again.
    version_file = os.path.join(clonedir, 'runtime', 'vm', 'version.cc')
    if os.path.exists(clonedir) and not os.path.exists(version_file):
        print('Delete incomplete clone directory ' + clonedir)
        def remove_readonly(func, path, _):
            os.chmod(path, stat.S_IWRITE)
            func(path)
        shutil.rmtree(clonedir, onerror=remove_readonly)
    
    # clone Dart source code
    if not os.path.exists(clonedir):
        # minimum clone repository at the target branch
        subprocess.run([GIT_CMD, '-c', 'advice.detachedHead=false', 'clone', '-b', info.version, '--depth', '1', '--filter=blob:none', '--sparse', '--progress', DART_GIT_URL, clonedir], check=True)
        # checkout only needed sources (runtime and tools)
        # since Dart 3.3 "third_party/double-conversion" is moved to outside of "runtime" directory
        subprocess.run([GIT_CMD, 'sparse-checkout', 'set', 'runtime', 'tools', 'third_party/double-conversion'], cwd=clonedir, check=True)
        # delete some unnecessary files
        with os.scandir(clonedir) as it:
            for entry in it:
                if entry.is_file():
                    os.remove(entry.path)
                elif entry.is_dir() and entry.name == '.git':
                    # should ".git" directory be removed?
                    pass

        if info.snapshot_hash is None:
            # if running with Python 3.12, tools/utils.py should be patched to replace imp module with importlib
            # due to its remotion as stated in: https://docs.python.org/3.12/whatsnew/3.12.html#imp
            if sys.version_info[:2] >= (3, 12):
                utils_path = os.path.join(clonedir, "tools/utils.py")
                if os.path.exists(utils_path):
                    with open(utils_path, "r+") as f:
                        content = f.read()
                        # the python 3.12 warnings are fixed in https://github.com/dart-lang/sdk/commit/a100968232c7492ab0de26897ff019e5784d4d38
                        if r"match_against('^MAJOR (\d+)$', content)" in content:
                            # old Dart version
                            # replace invalid escape sequences strings with raw strings to avoid SyntaxWarning
                            # in future Python versions this warning will raise an error instead of warning
                            # as stated in: https://docs.python.org/3/whatsnew/3.12.html#other-language-changes
                            content = content.replace(" ' awk ", " r' awk ").replace("match_against('", "match_against(r'").replace("re.search('", "re.search(r'")
                            if "import imp\n" in content:
                                content = content.replace("import imp\n", imp_replace_snippet).replace("imp.load_source", "load_source")
                            f.seek(0)
                            f.truncate()
                            f.write(content)
            # make version
            subprocess.run([sys.executable, 'tools/make_version.py', '--output', 'runtime/vm/version.cc', '--input', 'runtime/vm/version_in.cc'], cwd=clonedir, check=True)
        else:
            subprocess.run([sys.executable, MAKE_VERSION_FILE, clonedir, info.snapshot_hash], check=True)
    
    return clonedir

def cmake_dart(info: DartLibInfo, target_dir: str):
    # On windows, need developer command prompt for x64 (can check with "cl" command)
    # create dartsdk/vx.y.z/CMakefile.list
    with open(CMAKE_TEMPLATE_FILE, 'r') as f:
        code = f.read()
    with open(os.path.join(target_dir, 'CMakeLists.txt'), 'w') as f:
        f.write(code.replace('VERSION_PLACE_HOLDER', info.version))

    # create dartsdk/vx.y.z/Config.cmake.in
    with open(os.path.join(target_dir, 'Config.cmake.in'), 'w') as f:
        f.write('@PACKAGE_INIT@\n\n')
        f.write('include ( "${CMAKE_CURRENT_LIST_DIR}/dartvmTarget.cmake" )\n\n')
    
    # generate source list
    subprocess.run([sys.executable, CREATE_SRCLIST_FILE, target_dir], check=True)
    # cmake -GNinja -Bout3.0.3 -DCMAKE_BUILD_TYPE=Release
    #   add -DTARGET_ARCH=x64 for analyzing x64 libapp.so
    #   add -DTARGET_OS=ios for analyzing ios App
    # Note: pointer compression feature is set from Flutter and no one change it when building app.
    #       so only one build of Dart runtime is enough
    builddir = os.path.join(BUILD_DIR, info.lib_name)
    subprocess.run([CMAKE_CMD, '-GNinja', '-B', builddir, f'-DTARGET_OS={info.os_name}', f'-DTARGET_ARCH={info.arch}', 
        f'-DCOMPRESSED_PTRS={1 if info.has_compressed_ptrs else 0}', '-DCMAKE_BUILD_TYPE=Release', '--log-level=NOTICE'], 
        cwd=target_dir, check=True)
    
    # build and install dart vm library to packages directory
    subprocess.run([NINJA_CMD], cwd=builddir, check=True)
    subprocess.run([CMAKE_CMD, '--install', '.'], cwd=builddir, check=True)

def fetch_and_build(info: DartLibInfo):
    outdir = checkout_dart(info)
    cmake_dart(info, outdir)

if __name__ == "__main__":
    ver = sys.argv[1]
    os_name = 'android' if len(sys.argv) < 3 else sys.argv[2]
    arch = 'arm64' if len(sys.argv) < 4 else sys.argv[3]
    snapshot_hash = None if len(sys.argv) < 5 else sys.argv[4]
    info = DartLibInfo(ver, os_name, arch, snapshot_hash=snapshot_hash)
    fetch_and_build(info)

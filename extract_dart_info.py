import io
import os
import re
import requests
import sys
import zipfile
import zlib
from struct import unpack

from elftools.elf.elffile import ELFFile
from elftools.elf.enums import ENUM_E_MACHINE 
from elftools.elf.sections import SymbolTableSection

MH_MAGIC_64 = b'\xcf\xfa\xed\xfe'
FAT_MAGIC = b'\xca\xfe\xba\xbe'  # a fat header is always stored big endian
FAT_CIGAM = b'\xbe\xba\xfe\xca'
FAT_MAGIC_64 = b'\xca\xfe\xba\xbf'  # fat_arch_64 entries, not supported
FAT_CIGAM_64 = b'\xbf\xba\xfe\xca'
LC_SYMTAB = 0x2
LC_SEGMENT_64 = 0x19
N_STAB = 0xe0
N_TYPE = 0x0e
N_SECT = 0x0e
CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000c


def is_macho(path):
    with open(path, 'rb') as f:
        return f.read(4) in (MH_MAGIC_64, FAT_MAGIC, FAT_CIGAM)


class MachO:
    """Just enough Mach-O to find a symbol and read the bytes at it.

    An iOS application has no libapp.so/libflutter.so. It ships
    App.framework/App and Flutter.framework/Flutter instead, both Mach-O.
    """

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()

        self.slice_off = self._find_slice()
        magic = self.data[self.slice_off:self.slice_off + 4]
        assert magic == MH_MAGIC_64, f'Unsupported Mach-O image: {magic.hex()}'

        _, _, _, _, ncmds, _, _, _ = unpack('<IiiIIIII', self._read(0, 32))
        self.segments = []  # (vmaddr, vmsize, fileoff, filesize)
        self.symtab = None  # (symoff, nsyms, stroff, strsize)
        off = 32
        for _ in range(ncmds):
            cmd, cmdsize = unpack('<II', self._read(off, 8))
            if cmd == LC_SEGMENT_64:
                vmaddr, vmsize, fileoff, filesize = unpack('<QQQQ', self._read(off + 24, 32))
                self.segments.append((vmaddr, vmsize, fileoff, filesize))
            elif cmd == LC_SYMTAB:
                self.symtab = unpack('<IIII', self._read(off + 8, 16))
            off += cmdsize

    def _find_slice(self):
        # A 64-bit fat header uses 32-byte entries rather than 20, so parsing it
        # as a 32-bit one silently yields nonsense offsets. Say so instead.
        assert self.data[:4] not in (FAT_MAGIC_64, FAT_CIGAM_64), \
            'Universal binary with a 64-bit fat header (FAT_MAGIC_64) is not supported'
        if self.data[:4] not in (FAT_MAGIC, FAT_CIGAM):
            return 0
        # Prefer arm64, which is what Flutter ships for devices.
        count = unpack('>I', self.data[4:8])[0]
        fallback = None
        for i in range(count):
            cputype, _, offset, _, _ = unpack('>iIIII', self.data[8 + i * 20:28 + i * 20])
            if cputype == CPU_TYPE_ARM64:
                return offset
            if cputype == CPU_TYPE_X86_64 and fallback is None:
                fallback = offset
        assert fallback is not None, 'No arm64 or x64 image in the universal binary'
        return fallback

    def _read(self, offset, size):
        start = self.slice_off + offset
        return self.data[start:start + size]

    def vm_to_file(self, addr):
        for vmaddr, vmsize, fileoff, filesize in self.segments:
            if vmaddr <= addr < vmaddr + vmsize:
                return fileoff + (addr - vmaddr)
        return None

    def read_at_symbol(self, name, size):
        assert self.symtab is not None, 'Mach-O has no symbol table'
        symoff, nsyms, stroff, strsize = self.symtab
        wanted = name.encode()
        for i in range(nsyms):
            n_strx, n_type, _, _, n_value = unpack('<IBBHQ', self._read(symoff + i * 16, 16))
            if (n_type & N_STAB) != 0 or (n_type & N_TYPE) != N_SECT or n_strx >= strsize:
                continue
            end = self.data.index(b'\0', self.slice_off + stroff + n_strx)
            if self.data[self.slice_off + stroff + n_strx:end] == wanted:
                return self._read(self.vm_to_file(n_value), size)
        return None


def extract_snapshot_hash_flags_macho(app_file):
    macho = MachO(app_file)
    # snapshot header: magic, length and kind take 20 bytes, then the version
    # hash followed by the feature string, same as in an ELF libapp.so.
    data = macho.read_at_symbol('_kDartVmSnapshotData', 20 + 32 + 256)
    assert data is not None, 'Cannot find _kDartVmSnapshotData'
    snapshot_hash = data[20:52].decode()
    flags = data[52:]
    flags = flags[:flags.index(b'\0')].decode().strip().split(' ')
    return snapshot_hash, flags


def extract_flutter_framework_info(flutter_file):
    # The engine embeds the Dart VM version string, which names the target
    # directly, e.g. '3.10.7 (stable) (Tue Dec 23 ...) on "ios_arm64"'.
    macho = MachO(flutter_file)
    m = re.search(br'([\d][\w\.\-]*) \((?:stable|beta|dev)\) \([^)]*\) on "(ios|macos)_(arm64|x64)"', macho.data)
    assert m is not None, 'Cannot find the Dart version in the Flutter framework'
    dart_version = m.group(1).decode()
    os_name = m.group(2).decode()
    arch = m.group(3).decode()
    # engine ids are only used to look up an unknown Dart version
    return [], dart_version, arch, os_name


def extract_snapshot_hash_flags(libapp_file):
    with open(libapp_file, 'rb') as f:
        elf = ELFFile(f)
        # find "_kDartVmSnapshotData" symbol
        dynsym = elf.get_section_by_name('.dynsym')
        sym = dynsym.get_symbol_by_name('_kDartVmSnapshotData')[0]
        #section = elf.get_section(sym['st_shndx'])
        assert sym['st_size'] > 128
        f.seek(sym['st_value']+20)
        snapshot_hash = f.read(32).decode()
        data = f.read(256) # should be enough
        flags = data[:data.index(b'\0')].decode().strip().split(' ')
    
    return snapshot_hash, flags

def extract_libflutter_info(libflutter_file):
    with open(libflutter_file, 'rb') as f:
        elf = ELFFile(f)
        if elf.header.e_machine == 'EM_AARCH64': # 183
            arch = 'arm64'
        elif elf.header.e_machine == 'EM_IA_64': # 50
            arch = 'x64'
        else:
            assert False, f"Unsupport architecture: {elf.header.e_machine}"

        section = elf.get_section_by_name('.rodata')
        data = section.data()
        
        sha_hashes = re.findall(b'\x00([a-f\\d]{40})(?=\x00)', data)
        #print(sha_hashes)
        # all possible engine ids
        engine_ids = [ h.decode() for h in sha_hashes ]
        assert len(engine_ids) == 2, f'found hashes {", ".join(engine_ids)}'
        
        # beta/dev version of flutter might not use stable dart version (we can get dart version from sdk with found engine_id)
        # support stable, beta and dev channels
        m = re.search(br'\x00([\d\w\.-]+) \((stable|beta|dev)\)', data)
        if m is None:
            dart_version = None
        else:
            dart_version = m.group(1).decode()
        
    return engine_ids, dart_version, arch, 'android'

def get_dart_sdk_url_size(engine_ids):
    #url = f'https://storage.googleapis.com/dart-archive/channels/stable/release/3.0.3/sdk/dartsdk-windows-x64-release.zip'
    for engine_id in engine_ids:
        url = f'https://storage.googleapis.com/flutter_infra_release/flutter/{engine_id}/dart-sdk-windows-x64.zip'
        resp = requests.head(url)
        if resp.status_code == 200:
           sdk_size = int(resp.headers['Content-Length'])
           return engine_id, url, sdk_size
    
    return None, None, None

def get_dart_commit(url):
    # in downloaded zip
    # * dart-sdk/revision - the dart commit id of https://github.com/dart-lang/sdk/
    # * dart-sdk/version  - the dart version
    # revision and version zip file records should be in first 4096 bytes
    # using stream in case a server does not support range
    commit_id = None
    dart_version = None
    fp = None
    with requests.get(url, headers={"Range": "bytes=0-4096"}, stream=True) as r:
        if r.status_code // 10 == 20:
            x = next(r.iter_content(chunk_size=4096))
            fp = io.BytesIO(x)
    
    if fp is not None:
        while fp.tell() < 4096-30 and (commit_id is None or dart_version is None):
            #sig, ver, flags, compression, filetime, filedate, crc, compressSize, uncompressSize, filenameLen, extraLen = unpack(fp, '<IHHHHHIIIHH')
            _, _, _, compMethod, _, _, _, compressSize, _, filenameLen, extraLen = unpack('<IHHHHHIIIHH', fp.read(30))
            filename = fp.read(filenameLen)
            #print(filename)
            if extraLen > 0:
                fp.seek(extraLen, io.SEEK_CUR)
            data = fp.read(compressSize)
            
            # expect compression method to be zipfile.ZIP_DEFLATED
            assert compMethod == zipfile.ZIP_DEFLATED, 'Unexpected compression method'
            if filename == b'dart-sdk/revision':
                commit_id = zlib.decompress(data, wbits=-zlib.MAX_WBITS).decode().strip()
            elif filename == b'dart-sdk/version':
                dart_version = zlib.decompress(data, wbits=-zlib.MAX_WBITS).decode().strip()
    
    # TODO: if no revision and version in first 4096 bytes, get the file location from the first zip dir entries at the end of file (less than 256KB)
    return commit_id, dart_version

def extract_dart_info(libapp_file: str, libflutter_file: str):
    # An iOS application ships Mach-O images instead of libapp.so/libflutter.so
    if is_macho(libapp_file):
        snapshot_hash, flags = extract_snapshot_hash_flags_macho(libapp_file)
        _, dart_version, arch, os_name = extract_flutter_framework_info(libflutter_file)
        return dart_version, snapshot_hash, flags, arch, os_name

    snapshot_hash, flags = extract_snapshot_hash_flags(libapp_file)
    #print('snapshot hash', snapshot_hash)
    #print(flags)

    engine_ids, dart_version, arch, os_name = extract_libflutter_info(libflutter_file)
    # print('possible engine ids', engine_ids)
    # print('dart version', dart_version)

    if dart_version is None:
        engine_id, sdk_url, sdk_size = get_dart_sdk_url_size(engine_ids)
        # print(engine_id)
        # print(sdk_url)
        # print(sdk_size)

        commit_id, dart_version = get_dart_commit(sdk_url)
        # print(commit_id)
        # print(dart_version)
        #assert dart_version == dart_version_sdk
    
    # TODO: os (android or ios) and architecture (arm64 or x64)
    return dart_version, snapshot_hash, flags, arch, os_name


if __name__ == "__main__":
    libdir = sys.argv[1]
    libapp_file = os.path.join(libdir, 'libapp.so')
    libflutter_file = os.path.join(libdir, 'libflutter.so')

    print(extract_dart_info(libapp_file, libflutter_file))

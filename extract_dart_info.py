import io
import os
import re
import requests
import sys
import zipfile
import zlib
from struct import unpack

from elftools.elf.elffile import ELFFile

from macho import find_dart_version, find_dart_vm_snapshot_data, find_engine_ids, is_macho_file, load_macho_image


def extract_snapshot_hash_flags(libapp_file):
    if is_macho_file(libapp_file):
        image = load_macho_image(libapp_file, "arm64")
        assert not image.is_encrypted, "Mach-O image is encrypted. Use a decrypted iOS binary."
        data = find_dart_vm_snapshot_data(image)
        snapshot_hash = data[20:52].decode()
        flags = data[52:data.index(b'\0', 52)].decode().strip().split(' ')
        return snapshot_hash, flags

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
    if is_macho_file(libflutter_file):
        image = load_macho_image(libflutter_file, "arm64")
        assert not image.is_encrypted, "Mach-O image is encrypted. Use a decrypted iOS binary."
        data = image.data[image.offset:image.offset + image.size]
        engine_ids = find_engine_ids(data)
        dart_version = find_dart_version(data)
        assert len(engine_ids) > 0 or dart_version is not None, "Cannot find Flutter engine id or Dart version in Mach-O image"
        return engine_ids, dart_version, image.arch, 'ios'

    with open(libflutter_file, 'rb') as f:
        elf = ELFFile(f)
        if elf.header.e_machine == 'EM_AARCH64': # 183
            arch = 'arm64'
        elif elf.header.e_machine in ('EM_X86_64', 'EM_IA_64'): # 62, 50
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
    
    return dart_version, snapshot_hash, flags, arch, os_name


if __name__ == "__main__":
    libdir = sys.argv[1]
    libapp_file = os.path.join(libdir, 'libapp.so')
    libflutter_file = os.path.join(libdir, 'libflutter.so')
    if not os.path.exists(libapp_file):
        libapp_file = os.path.join(libdir, 'App')
    if not os.path.exists(libflutter_file):
        libflutter_file = os.path.join(libdir, 'Flutter')

    print(extract_dart_info(libapp_file, libflutter_file))

import sys
from struct import pack, unpack
from capstone import *
from elftools.elf.elffile import ELFFile

# Note: current only support AArch64 architecture
def extract_libflutter_functions(libflutter_file):
    with open(libflutter_file, 'rb') as f:
        elf = ELFFile(f)
        section = elf.get_section_by_name('.rodata')
        rodata = section.data()
        rohdr = section.header
        
        gerVersion_text_offset = rohdr.sh_addr + rodata.index(b'\x00Platform_GetVersion\x00') + 1
        print(f'Platform_GetVersion text offset: {gerVersion_text_offset:#x}')
        gerVersion_text_offset = pack('<Q', gerVersion_text_offset)
        
        section = elf.get_section_by_name('.rela.dyn')
        rela_data = section.data()
        rela_entry_offset = rela_data.index(gerVersion_text_offset) - 16
        assert rela_entry_offset % 24 == 0, rela_entry_offset
        print(f'Platform_GetVersion text rela offset: {section.header.sh_addr+rela_entry_offset:#x}')
        
        def getRefString(addr):
            assert addr > rohdr.sh_addr
            offset = addr - rohdr.sh_addr
            assert offset < rohdr.sh_size
            epos = rodata.index(b'\x00', offset)
            return rodata[offset:epos].decode()
        
        while (True):
            # normally, compiler put the rela entry in order
            # one entry for function name, and the next one is function entry point
            rela_entry_offset -= 24 * 2  # 24 is rela entry size
            str_addr = unpack('<Q', rela_data[rela_entry_offset+16:rela_entry_offset+24])[0]
            name = getRefString(str_addr)
            if name == 'Crypto_GetRandomBytes':
                break

        io_natives = {}
        while (True):
            # normally, compiler put the rela entry in order
            # one entry for function name, and the next one is function entry point
            str_addr = unpack('<Q', rela_data[rela_entry_offset+16:rela_entry_offset+24])[0]
            name = getRefString(str_addr)
            rela_entry_offset += 24  # 24 is rela entry size
            fn_addr = unpack('<Q', rela_data[rela_entry_offset+16:rela_entry_offset+24])[0]
            rela_entry_offset += 24  # 24 is rela entry size
            io_natives[name] = fn_addr
            if name == 'SystemEncodingToString':
                break
        
        text_hdr = elf.get_section_by_name('.text').header
        def readCode(addr, size):
            elf.stream.seek(addr - text_hdr.sh_addr + text_hdr.sh_offset)
            return elf.stream.read(size)
        
        dart_fns = {}
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        # get version
        fn_addr = io_natives['Platform_GetVersion']
        print(f'Platform_GetVersion: {fn_addr:#x}')
        getVersion_code = readCode(fn_addr, 40)
        code = list(md.disasm_lite(getVersion_code, fn_addr))
        assert code[0][2] == 'stp', code[0]
        assert code[1][2] == 'mov'
        assert code[2][2] == 'adrp', code[2]
        dart_version_addr = int(code[2][3][5:], 0)
        assert code[3][2] == 'add' and code[3][3].startswith('x0, x0, #'), code[3]
        dart_version_addr += int(code[3][3][9:], 0)
        dart_version = getRefString(dart_version_addr)
        assert code[4][2] == 'bl', code[4]
        dart_fns['Dart_NewStringFromCString'] = int(code[4][3][1:], 0)
        assert code[5][2] == 'mov', code[5]
        assert code[6][2] == 'mov', code[6]
        assert code[7][2] == 'ldp', code[7]
        assert code[8][2] == 'b', code[8]
        dart_fns['Dart_SetReturnValue'] = int(code[8][3][1:], 0)
        
        
        fn_addr = io_natives['Stdout_GetTerminalSize']
        print(f'Stdout_GetTerminalSize: {fn_addr:#x}')
        raw_code = readCode(fn_addr, 0x100)
        offset = raw_code.index(b'\x40\x00\x80\x52') + 4 # mov w0, #2  ; set size of list
        assert offset % 4 == 0
        for (_, _, mnemonic, op_str) in md.disasm_lite(raw_code[offset:], fn_addr+offset):
            if mnemonic != 'bl':
                continue
            # first BL is Dart_NewList(size)
            if 'Dart_NewList' not in dart_fns:
                dart_fns['Dart_NewList'] = int(op_str[1:], 0)
            elif 'Dart_NewInteger' not in dart_fns:
                dart_fns['Dart_NewInteger'] = int(op_str[1:], 0)
            elif 'Dart_ListSetAt' not in dart_fns:
                dart_fns['Dart_ListSetAt'] = int(op_str[1:], 0)
                break
        
        
        print(dart_version)
        for name, addr in dart_fns.items():
            print(f'{name}: {addr:#x}')
        #for name, addr in io_natives.items():
        #    print(f'{name}: {addr:#x}')
        
    return dart_version

if __name__ == "__main__":
    libflutter = sys.argv[1]
    extract_libflutter_functions(libflutter)

#!/usr/bin/env python3
# This script parses the offset at runtime/vm/thread.h
import sys
import re

hdr_file = sys.argv[1]
with open(hdr_file, 'r') as f:
    content = f.read()


names = re.findall(r'\sOFFSET_OF\(Thread, (\w+?)_\);', content)
for name in names:
    if name.startswith('ffi_'):
        method = name[4:]
    elif name.startswith('thread_'):
        method = name[7:]
        name = method
    else:
        method = name
    print(f'threadOffsetNames[dart::Thread::{method}_offset()] = "{name}";')

#!/usr/bin/env python3
# Setup required libraries for compiling blutter
# Note: for Windows only
import io
import os
import requests
import shutil
import zipfile
from pathlib import Path

ICU_LIB_URL = 'https://github.com/unicode-org/icu/releases/download/release-73-2/icu4c-73_2-Win64-MSVC2019.zip'
# Note: capstone 5 has no pre-built yet
CAPSTONE_LIB_URL = 'https://github.com/capstone-engine/capstone/releases/download/4.0.2/capstone-4.0.2-win64.zip'

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
BIN_DIR = os.path.join(SCRIPT_DIR, '..', 'bin')
EXTERNAL_DIR = os.path.join(SCRIPT_DIR, '..', 'external')
ICU_WINDOWS_FILE = os.path.join(EXTERNAL_DIR, 'icu-windows.zip')
ICU_WINDOWS_DIR = os.path.join(EXTERNAL_DIR, 'icu-windows')
CAPSTONE_DIR = os.path.join(EXTERNAL_DIR, 'capstone')

Path(BIN_DIR).mkdir(parents=True, exist_ok=True)
Path(EXTERNAL_DIR).mkdir(parents=True, exist_ok=True)
if os.path.exists(CAPSTONE_DIR):
    shutil.rmtree(CAPSTONE_DIR)

# icu
print('Downloading ICU library from ' + ICU_LIB_URL)
r = requests.get(ICU_LIB_URL)
print('Extracting ICU library')
with zipfile.ZipFile(io.BytesIO(r.content)) as z:
    with z.open(z.namelist()[-1]) as zf, open(ICU_WINDOWS_FILE, 'wb') as f:
        shutil.copyfileobj(zf, f)

with zipfile.ZipFile(ICU_WINDOWS_FILE) as z:
    z.extractall(ICU_WINDOWS_DIR)
os.remove(ICU_WINDOWS_FILE)

# capstone
print('Downloading Capstone from ' + CAPSTONE_LIB_URL)
r = requests.get(CAPSTONE_LIB_URL)
print('Extracting Capstone library')
with zipfile.ZipFile(io.BytesIO(r.content)) as z:
    capstone_zip_dir = z.namelist()[0].split('/', 1)[0]
    z.extractall(EXTERNAL_DIR)

os.rename(os.path.join(EXTERNAL_DIR, capstone_zip_dir), CAPSTONE_DIR)

# copy to bin (version of icu dll MUST be updated)
print('Copying dlls to bin directory')
shutil.copy(os.path.join(CAPSTONE_DIR, 'capstone.dll'), BIN_DIR)
shutil.copy(os.path.join(ICU_WINDOWS_DIR, 'bin64', 'icudt73.dll'), BIN_DIR)
shutil.copy(os.path.join(ICU_WINDOWS_DIR, 'bin64', 'icuuc73.dll'), BIN_DIR)

print('Done')

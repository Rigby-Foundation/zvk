#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Build the pinned GLSL fixture compiler locally; never link it into zvk."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

root=Path(__file__).resolve().parents[1]
build=root/'build/tools'
build.mkdir(parents=True,exist_ok=True)
archive=build/'glslang.tar.gz'
url='https://codeload.github.com/KhronosGroup/glslang/tar.gz/refs/tags/15.1.0'
digest='4bdcd8cdb330313f0d4deed7be527b0ac1c115ff272e492853a6e98add61b4bc'
if not archive.is_file():
    print('Fetching '+url,flush=True)
    with urllib.request.urlopen(url,timeout=60) as response,archive.open('wb') as out:
        shutil.copyfileobj(response,out)
if hashlib.sha256(archive.read_bytes()).hexdigest()!=digest:
    raise SystemExit('Checksum mismatch: '+str(archive))
source=build/'glslang-15.1.0'
if not source.is_dir():
    with tarfile.open(archive) as tar:tar.extractall(build,filter='data')
cmake=shutil.which('cmake')
if not cmake:
    cmake=str(build/'python/cmake/data/bin/cmake')
    if not Path(cmake).is_file():
        subprocess.run([sys.executable,'-m','pip','install','--target',str(build/'python'),'cmake==4.4.4'],check=True)
args=[cmake,'-S',str(source),'-B',str(build/'glslang-build'),'-DCMAKE_BUILD_TYPE=Release',
      '-DENABLE_OPT=OFF','-DENABLE_HLSL=OFF','-DBUILD_TESTING=OFF','-DENABLE_GLSLANG_BINARIES=ON']
if shutil.which('ninja'):args+=['-G','Ninja']
subprocess.run(args,check=True)
subprocess.run([cmake,'--build',str(build/'glslang-build'),'-j',str(min(os.cpu_count() or 2,8))],check=True)
print('Fixture compiler: '+str(build/'glslang-build/StandAlone/glslang'))

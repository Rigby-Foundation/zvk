#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Compile real Vulkan GLSL fixtures and embed them in a self-contained test."""
from pathlib import Path
import os, shutil, subprocess, struct
root=Path(__file__).resolve().parents[1]
tool=os.environ.get('GLSLANG') or shutil.which('glslangValidator') or shutil.which('glslang')
if not tool:
    tool=str(root/'build/tools/glslang-build/StandAlone/glslang')
if not Path(tool).is_file():
    raise SystemExit('glslang not found; run make tools, or set GLSLANG=/path/to/glslang')
out=root/'build/shaders';out.mkdir(parents=True,exist_ok=True)
lines=['/* Generated from tests/shaders; do not edit. */']
for path in sorted((root/'tests/shaders').glob('*')):
    if path.suffix not in ('.vert','.frag','.comp','.spvasm'):continue
    dest=out/(path.name+'.spv')
    if path.suffix=='.spvasm':
        subprocess.run(['spirv-as','--preserve-numeric-ids','--target-env','vulkan1.0','-o',str(dest),str(path)],check=True)
    else:
        subprocess.run([tool,'-V','--target-env','vulkan1.0','-o',str(dest),str(path)],check=True)
    if shutil.which('spirv-val'):subprocess.run(['spirv-val','--target-env','vulkan1.0',str(dest)],check=True)
    data=dest.read_bytes();words=struct.unpack('<%dI'%(len(data)//4),data)
    name=path.name.replace('.','_')
    lines.append('static const uint32_t '+name+'[] = {')
    for i in range(0,len(words),8):lines.append('    '+','.join('0x%08xu'%x for x in words[i:i+8])+',')
    lines.append('};')
(root/'build/shaders.h').write_text('\n'.join(lines)+'\n')

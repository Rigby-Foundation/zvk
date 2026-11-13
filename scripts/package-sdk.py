#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Package the native shared Vulkan/SDL SDK; do not mount or alter any disk."""
import argparse, shutil, tarfile
from copyutil import copy2
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--arch',choices=['aarch64','x86_64'],default='aarch64');p.add_argument('--sysroot',type=Path,required=True);a=p.parse_args()
stage=root/'build'/('sdk-'+a.arch)
if stage.exists():shutil.rmtree(stage)
def copy(source,path):
    target=stage/path;target.parent.mkdir(parents=True,exist_ok=True);copy2(source,target)
for directory in ['vulkan','vk_video','SDL2']:
    shutil.copytree(a.sysroot/'usr/include'/directory,stage/'usr/include'/directory,copy_function=copy2)
for name in ['zvk.h','zvk_zwm.h','zwm.h']:copy(a.sysroot/'usr/include'/name,'usr/include/'+name)
for name in ['libzvk.so','libvirgl.so','libzwm.so','libzgl.so','libvulkan.so.1','libSDL2.so','libc.so','Scrt1.o','crti.o','crtn.o','libcompiler_rt.a']:
    copy(a.sysroot/'usr/lib'/name,'usr/lib/'+name)
(stage/'usr/lib/libvulkan.so').symlink_to('libvulkan.so.1')
copy(a.sysroot/'usr/lib/libc.so','lib/ld-musl-'+a.arch+'.so.1')
copy(a.sysroot/'usr/share/vulkan/icd.d/zvk.json','usr/share/vulkan/icd.d/zvk.json')
for name in ['sdlwindow','zvkwindow','zvktest','wsitest','zvktriangle']:copy(root/'build'/a.arch/name,'bin/'+name)
copy(root.parent/'zwm/build'/a.arch/'zwm','bin/zwm')
examples='usr/share/zvk/examples/'
for name in ['window.c','window_sdl.h','triangle.c','compute.c','Makefile.sic','stamp-elf.c']:copy(root/'examples'/name,examples+name)
copy(root/'build/shaders.h',examples+'shaders.h')
for source,name in [('loaderprobe.c','loaderprobe.c'),('dynamic/smoke.c','dynamic-smoke.c'),('dynamic/plugin.c','plugin.c')]:copy(root/'tests'/source,examples+name)
for source,name in [(root/'vendor/bcdec/LICENSE','bcdec-LICENSE'),(root/'vendor/bcdec/README.md','bcdec-README.md'),(root/'vendor/bcdec/portability.patch','bcdec-portability.patch'),(root/'CONFORMANCE.md','CONFORMANCE.md'),(root/'README.md','README.md'),(root/'NOTICE','NOTICE'),(root.parent/'libc/COPYRIGHT','musl-COPYRIGHT'),(root.parent/'libc/NOTICE','sic-musl-NOTICE'),(root.parent/'LICENSE','LICENSE'),(root.parent/'zgl/LICENSE.LGPL','LICENSE.LGPL'),(root/'build/tools/Vulkan-Loader-sic/LICENSE.txt','Vulkan-Loader-LICENSE.txt'),(root.parent/'zde/ports/sdl2/src/LICENSE.txt','SDL2-LICENSE.txt')]:copy(source,'usr/share/doc/zvk/'+name)
zig=shutil.which('zig')
if zig:
    license=Path(zig).resolve().parents[1]/'LICENSE'
    if license.is_file():copy(license,'usr/share/doc/zvk/Zig-LICENSE')
archive=root/'build'/('zvk-sdk-'+a.arch+'.tar')
with tarfile.open(archive,'w') as tar:
    for path in sorted(stage.iterdir()):tar.add(path,arcname=path.name)
print('Native SDK (extract at / inside sic): '+str(archive))

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Build ELF/PIC zVk and its GPU/zwm dependencies, independently of static SDK."""
import argparse, hashlib, json, subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];repo=root.parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--arch',choices=['aarch64','x86_64'],default='aarch64');p.add_argument('--sysroot',type=Path);a=p.parse_args()
out=root/'build'/a.arch;pic=out/'pic';pic.mkdir(parents=True,exist_ok=True)
sysroot=(a.sysroot or repo/('sysroot-aarch64' if a.arch=='aarch64' else 'sysroot')).resolve()
cc=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin/clang'
ld=Path(subprocess.check_output(['brew','--prefix','lld@21'],text=True).strip())/'bin/ld.lld'
flags=[f'--target={a.arch}-linux-musl','-std=c11','-fPIC','-O2','-g','-D_GNU_SOURCE','-D__sic__','-fno-stack-protector','-nostdinc','-isystem',str(sysroot/'usr/include')]
includes=[root/'build',root/'include',root/'src',root/'build/deps/vulkan-headers/include',root/'build/deps/spirv-headers/include',repo/'zgl/include',repo/'zgl/lib',repo/'zwm/include']
flags += ['-I'+str(path) for path in includes]
# Compile only changed inputs; header/flags changes invalidate every PIC object.
h=hashlib.sha256(' '.join(flags).encode())
for folder in includes:
    for path in sorted(folder.glob('*.h' if folder==root/'build' else '**/*.h')):h.update(path.read_bytes())
for path in sorted((root/'vendor').glob('**/*.h')):h.update(path.read_bytes())
digest=h.hexdigest();stamp=pic/'.flags';dirty=not stamp.exists() or stamp.read_text()!=digest

def library(name,sources,deps=(),extra=()):
    objects=[]
    for source in sources:
        obj=pic/(str(source.relative_to(repo)).replace('/','_')+'.o');objects.append(obj)
        if dirty or not obj.exists() or source.stat().st_mtime>obj.stat().st_mtime:
            subprocess.run([str(cc),*flags,'-c',str(source),'-o',str(obj)],check=True)
    target=out/name
    dependencies=[out/('lib'+name+'.so') for name in deps]
    if dirty or not target.exists() or any(path.stat().st_mtime>target.stat().st_mtime for path in [*objects,*dependencies]):
        subprocess.run([str(ld),'-shared','-z','defs','-z','max-page-size=4096','-soname',name,'-o',str(target),*extra,*map(str,objects),'-L'+str(out),*['-l'+d for d in deps],'-L'+str(sysroot/'usr/lib'),'-lc',str(sysroot/'usr/lib/libcompiler_rt.a')],check=True)
    print(target)

library('libzwm.so',sorted((repo/'zwm/lib').glob('*.c')))
library('libvirgl.so',[repo/'zgl/lib/virgl.c',*sorted((repo/'zgl/lib/r300').glob('*.c')),*sorted((repo/'zgl/lib/adreno').glob('*.c'))])
library('libzgl.so',sorted((repo/'zgl/lib/gl').glob('*.c')),['virgl','zwm'])
# Bind the ICD's vk entry points locally, preventing loader symbol interposition.
library('libzvk.so',sorted((root/'src').glob('*.c')),['virgl','zwm'],['-Bsymbolic'])
stamp.write_text(digest)
(out/'zvk_icd.json').write_text(json.dumps({'file_format_version':'1.0.0','ICD':{'library_path':'../../../lib/libzvk.so','api_version':'1.0.0'}},indent=2)+'\n')

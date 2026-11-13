#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Build isolated, pinned Khronos loader and validation tools for Mac testing."""
import hashlib,json,os,shutil,subprocess,sys,tarfile,urllib.request
from pathlib import Path
from loader_fixes import apply as fix_loader
root=Path(__file__).resolve().parents[1];build=root/'build/tools';prefix=build/'vulkan-sdk'
if sys.platform!='darwin':raise SystemExit('The current host transport/ICD test target is for macOS.')
cmake=shutil.which('cmake') or str(build/'python/cmake/data/bin/cmake')
if not Path(cmake).is_file():raise SystemExit('Run make tools first to supply CMake.')
env=os.environ.copy();env['PATH']=str(Path(cmake).parent)+os.pathsep+env['PATH'];env['CMAKE_BUILD_PARALLEL_LEVEL']='6'
for name,repository,digest in [
 ('loader','Vulkan-Loader','682d5323cf31308402c888599b375ebf15810f95d6d1a08ad2f525766becf99b'),
 ('validation','Vulkan-ValidationLayers','99a4047b82b91b7856cbfaf2fe1629c3aa436ed2300a5e64d820b97d8c685564')]:
    archive=build/(name+'-1.3.296.tar.gz')
    if not archive.is_file():
        url='https://codeload.github.com/KhronosGroup/'+repository+'/tar.gz/refs/tags/v1.3.296'
        print('Fetching '+url,flush=True)
        temp=archive.with_suffix('.part')
        with urllib.request.urlopen(url,timeout=60) as response,temp.open('wb') as out:shutil.copyfileobj(response,out)
        temp.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=digest:raise SystemExit('Checksum mismatch: '+str(archive))
    source=build/(repository+'-1.3.296')
    if not source.is_dir():
        with tarfile.open(archive) as tar:tar.extractall(build,filter='data')
fix_loader(build/'Vulkan-Loader-1.3.296')
# Resolve the release's known-good dependency tags to immutable commits.
pins={'Vulkan-Headers':'29f979ee5aa58b7b005f805ea8df7a855c39ff37','Vulkan-Utility-Libraries':'a71907b8577c994b1c69adfa7315d55bbe79656e','SPIRV-Headers':'2a9b6f951c7d6b04b6c21fe1bf3f475b68b84801','SPIRV-Tools':'6dcc7e350a0b9871a825414d42329e44b0eb8109','robin-hood-hashing':'9145f963d80d6a02f0f96a47758050a89184a3ed'}
known_good=build/'Vulkan-ValidationLayers-1.3.296/scripts/known_good.json';data=json.loads(known_good.read_text())
changed=False
for repo in data['repos']:
    if repo['name'] in pins and repo['commit']!=pins[repo['name']]:repo['commit']=pins[repo['name']];changed=True
if changed:known_good.write_text(json.dumps(data,indent=4)+'\n')
def run(*args):subprocess.run([cmake,*map(str,args)],env=env,check=True)
run('-S',root/'build/deps/vulkan-headers','-B',build/'vulkan-headers-build','-G','Ninja','-DCMAKE_INSTALL_PREFIX='+str(prefix),'-DVULKAN_HEADERS_ENABLE_TESTS=OFF')
run('--install',build/'vulkan-headers-build')
common=['-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_INSTALL_PREFIX='+str(prefix),'-DCMAKE_PREFIX_PATH='+str(prefix),'-DBUILD_TESTS=OFF','-DBUILD_WSI_XCB_SUPPORT=OFF','-DBUILD_WSI_XLIB_SUPPORT=OFF','-DBUILD_WSI_WAYLAND_SUPPORT=OFF','-DBUILD_WSI_DIRECTFB_SUPPORT=OFF']
for source,dest,extra in [('Vulkan-Loader-1.3.296','vulkan-loader-build',[]),('Vulkan-ValidationLayers-1.3.296','vulkan-validation-build',['-DUPDATE_DEPS=ON'])]:
    run('-S',build/source,'-B',build/dest,*common,*extra)
    run('--build',build/dest,'-j6');run('--install',build/dest)
print('Local Vulkan loader/layers: '+str(prefix))

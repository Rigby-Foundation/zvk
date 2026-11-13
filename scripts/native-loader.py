#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Build the pinned Khronos ELF loader with sic's private zwm surface extension.
Registry additions are regenerated with Khronos' generator, never hand-edited
into dispatch tables. The macOS validation-tool source/build stays untouched.
"""
import argparse, copy, hashlib, os, shutil, subprocess, sys, tarfile, urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path
from loader_fixes import apply as fix_loader
root=Path(__file__).resolve().parents[1]; repo=root.parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--arch',choices=['aarch64','x86_64'],default='aarch64');p.add_argument('--sysroot',type=Path);a=p.parse_args()
build=root/'build/tools'; build.mkdir(parents=True,exist_ok=True)
archive=build/'loader-1.3.296.tar.gz'
if not archive.exists():
    with urllib.request.urlopen('https://codeload.github.com/KhronosGroup/Vulkan-Loader/tar.gz/refs/tags/v1.3.296',timeout=60) as r,archive.open('wb') as out:shutil.copyfileobj(r,out)
if hashlib.sha256(archive.read_bytes()).hexdigest()!='682d5323cf31308402c888599b375ebf15810f95d6d1a08ad2f525766becf99b':raise SystemExit('Loader checksum mismatch')
source=build/'Vulkan-Loader-sic'; stamp=source/'.sic-port'
digest=hashlib.sha256(Path(__file__).read_bytes()+Path(__file__).with_name('loader_fixes.py').read_bytes()).hexdigest()
if not stamp.exists() or stamp.read_text()!=digest:
    if source.exists():shutil.rmtree(source)
    source.mkdir()
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            member.name=member.name.partition('/')[2]
            if member.name:tar.extract(member,source,filter='data')
    fix_loader(source)
    registry=source/'registry';shutil.copytree(root/'build/deps/vulkan-headers/registry',registry)
    tree=ET.parse(registry/'vk.xml');xml=tree.getroot()
    commands=xml.find('commands'); cmd=copy.deepcopy(next(c for c in commands if c.findtext('proto/name')=='vkCreateHeadlessSurfaceEXT'))
    for node in cmd.iter():
        if node.text:node.text=node.text.replace('HeadlessSurfaceEXT','ZwmSurfaceSIC')
    commands.append(cmd)
    types=xml.find('types');typ=copy.deepcopy(next(t for t in types if t.get('name')=='VkHeadlessSurfaceCreateInfoEXT'))
    typ.set('name','VkZwmSurfaceCreateInfoSIC')
    for node in typ.iter():
        if node.text:node.text=node.text.replace('HeadlessSurfaceCreateFlagsEXT','Flags')
        if node.get('values'):node.set('values',node.get('values').replace('HEADLESS_SURFACE_CREATE_INFO_EXT','ZWM_SURFACE_CREATE_INFO_SIC'))
    # Full XML member declarations are needed by the object-types generator.
    member=ET.SubElement(typ,'member');ET.SubElement(member,'type').text='void';member[0].tail=' *';ET.SubElement(member,'name').text='connection'
    member=ET.SubElement(typ,'member');ET.SubElement(member,'type').text='uint32_t';member[0].tail=' ';ET.SubElement(member,'name').text='window'
    # Only command parameter types enter the loader generator's tables.
    types.append(typ)
    ext=copy.deepcopy(next(e for e in xml.find('extensions') if e.get('name')=='VK_EXT_headless_surface'))
    ext.set('name','VK_SIC_zwm_surface');ext.set('number','1000');ext.attrib.pop('author',None);ext.attrib.pop('contact',None)
    for node in ext.iter():
        for key,val in list(node.attrib.items()):node.set(key,val.replace('VK_EXT_headless_surface','VK_SIC_zwm_surface').replace('HEADLESS_SURFACE','SIC_ZWM_SURFACE').replace('VK_EXT_SIC_ZWM','VK_SIC_ZWM').replace('HeadlessSurfaceEXT','ZwmSurfaceSIC').replace('HeadlessSurfaceCreateInfoEXT','ZwmSurfaceCreateInfoSIC').replace('HeadlessSurfaceCreateFlagsEXT','Flags'))
    # sType spelling in our private header differs from the extension macro.
    for node in ext.iter():
        if node.get('name')=='VK_STRUCTURE_TYPE_SIC_ZWM_SURFACE_CREATE_INFO_EXT':node.set('name','VK_STRUCTURE_TYPE_ZWM_SURFACE_CREATE_INFO_SIC')
    xml.find('extensions').append(ext);tree.write(registry/'vk.xml',encoding='unicode')
    def edit(path,old,new):
        f=source/path;s=f.read_text()
        if old not in s:raise SystemExit('Pinned loader port context changed: '+str(path))
        f.write_text(s.replace(old,new))
    edit('scripts/loader_extension_generator.py',"'VK_EXT_headless_surface',","'VK_EXT_headless_surface', 'VK_SIC_zwm_surface',")
    edit('loader/loader_common.h','bool wsi_headless_surface_enabled;','bool wsi_headless_surface_enabled;\n    bool wsi_zwm_surface_enabled;')
    wsi=(source/'loader/wsi.c').read_text();start=wsi.index('LOADER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateHeadlessSurfaceEXT(');end=wsi.index('// Ensure we are properly',start)
    block=wsi[start:end].replace('HeadlessSurfaceEXT','ZwmSurfaceSIC').replace('HeadlessSurfaceCreateInfoEXT','ZwmSurfaceCreateInfoSIC').replace('headless_surface','zwm_surface').replace('VK_EXT_zwm_surface','VK_SIC_zwm_surface')
    # Upstream headless's disabled-extension early return holds loader_lock.
    block=block.replace('return VK_SUCCESS;','loader_platform_thread_unlock_mutex(&loader_lock);\n        return VK_ERROR_EXTENSION_NOT_PRESENT;')
    edit('loader/wsi.c','// Ensure we are properly',block+'// Ensure we are properly')
    edit('loader/wsi.c','        if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)', '        if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], VK_SIC_ZWM_SURFACE_EXTENSION_NAME) == 0) {\n            loader_inst->wsi_zwm_surface_enabled = true;\n            continue;\n        }\n        if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)')
    edit('loader/wsi.c','    // Functions for the VK_EXT_headless_surface extension:', '    if (!strcmp("vkCreateZwmSurfaceSIC", name)) {\n        *addr = loader_inst->wsi_zwm_surface_enabled ? (void *)vkCreateZwmSurfaceSIC : NULL;\n        return true;\n    }\n    // Functions for the VK_EXT_headless_surface extension:')
    edit('loader/wsi.h','VKAPI_ATTR VkResult VKAPI_CALL terminator_CreateHeadlessSurfaceEXT(', 'VKAPI_ATTR VkResult VKAPI_CALL terminator_CreateZwmSurfaceSIC(VkInstance, const VkZwmSurfaceCreateInfoSIC *, const VkAllocationCallbacks *, VkSurfaceKHR *);\n\nVKAPI_ATTR VkResult VKAPI_CALL terminator_CreateHeadlessSurfaceEXT(')
    subprocess.run([sys.executable,str(source/'scripts/generate_source.py'),str(registry)],check=True)
    stamp.write_text(digest)
cmake=shutil.which('cmake') or str(build/'python/cmake/data/bin/cmake')
llvm=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip());lld=Path(subprocess.check_output(['brew','--prefix','lld@21'],text=True).strip())/'bin/ld.lld'
sysroot=(a.sysroot or repo/('sysroot-aarch64' if a.arch=='aarch64' else 'sysroot')).resolve()
flags=f'--target={a.arch}-linux-musl --sysroot={sysroot} -D_GNU_SOURCE -D__sic__ -I{root}/include -include {root}/include/zvk_zwm.h -fno-stack-protector'
# Use clang's ELF linker without Darwin's SDK/startup objects. All POSIX calls
# go through sic musl; no host or Linux runtime library enters the build.
link=f'--ld-path={lld} -nostdlib -L{sysroot}/usr/lib -Wl,-z,max-page-size=4096 -lc {sysroot}/usr/lib/libcompiler_rt.a'
headers_prefix=build/'native-vulkan-headers'
subprocess.run([cmake,'-S',str(root/'build/deps/vulkan-headers'),'-B',str(build/'native-vulkan-headers-build'),'-G','Ninja','-DCMAKE_INSTALL_PREFIX='+str(headers_prefix),'-DVULKAN_HEADERS_ENABLE_TESTS=OFF'],check=True)
subprocess.run([cmake,'--install',str(build/'native-vulkan-headers-build')],check=True)
folder=build/('vulkan-loader-sic-'+a.arch)
subprocess.run([cmake,'-S',str(source),'-B',str(folder),'-G','Ninja','-DCMAKE_SYSTEM_NAME=Linux','-DCMAKE_SYSTEM_PROCESSOR='+a.arch,'-DCMAKE_C_COMPILER='+str(llvm/'bin/clang'),'-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY','-DCMAKE_AR='+str(llvm/'bin/llvm-ar'),'-DCMAKE_RANLIB='+str(llvm/'bin/llvm-ranlib'),'-DCMAKE_C_FLAGS='+flags,'-DCMAKE_SHARED_LINKER_FLAGS='+link,'-DCMAKE_EXE_LINKER_FLAGS='+link,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_PREFIX_PATH='+str(headers_prefix),'-DVULKAN_HEADERS_INSTALL_DIR='+str(root/'build/deps/vulkan-headers'),'-DUSE_GAS=OFF','-DBUILD_TESTS=OFF','-DBUILD_WSI_XCB_SUPPORT=OFF','-DBUILD_WSI_XLIB_SUPPORT=OFF','-DBUILD_WSI_WAYLAND_SUPPORT=OFF','-DBUILD_WSI_DIRECTFB_SUPPORT=OFF'],check=True)
subprocess.run([cmake,'--build',str(folder),'-j6'],check=True)
out=root/'build'/a.arch;out.mkdir(parents=True,exist_ok=True)
shutil.copy2(folder/'loader/libvulkan.so.1.3.296',out/'libvulkan.so.1')
print('sic Vulkan loader: '+str(out/'libvulkan.so.1'))

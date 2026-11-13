#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the official, pinned macOS Vulkan CTS without global SDK changes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'build/cts'
VERSION = '1.4.7.0'
SHA256 = 'f6cc5bf35adf6e6260444fe5ecbd3d74dfc74915a7779f8c0d7d0c95316bf51e'


def source(url, digest, archive, dest, select=lambda path: True):
    if not archive.is_file():
        print('Fetching ' + url, flush=True)
        part = archive.with_suffix('.part')
        with urllib.request.urlopen(url, timeout=120) as src, part.open('wb') as out:
            shutil.copyfileobj(src, out)
        part.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise SystemExit('SHA256 mismatch: ' + str(archive))
    if dest.is_dir():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        prefix = tar.getmembers()[0].name.split('/')[0]
        for member in tar.getmembers():
            path = Path(*Path(member.name).parts[1:])
            if select(path):
                tar.extract(member, dest.parent, filter='data')
    (dest.parent / prefix).rename(dest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        raise SystemExit('The current zVk host/CTS integration is for macOS.')
    BASE.mkdir(parents=True, exist_ok=True)
    src = BASE / 'source'
    source('https://codeload.github.com/KhronosGroup/VK-GL-CTS/tar.gz/refs/tags/vulkan-cts-' + VERSION,
           SHA256, BASE / ('vulkan-cts-' + VERSION + '.tar.gz'), src,
           lambda path: not path.parts or path.parts[0] != 'android')
    pins = json.loads(Path(__file__).with_name('cts-dependencies.json').read_text())
    for name, pin in pins.items():
        select = (lambda path: not path.parts or path.parts[0] in ('xml', 'LICENSE.md', 'LICENSE.txt')) \
            if name == 'vulkan-docs' else lambda path: True
        source(pin['url'], pin['sha256'], BASE / (name + '-' + pin['commit'] + '.tar.gz'),
               src / 'external' / name / 'src', select)
    # Build-only fix: modern libc++ needs <cmath> for the std math names
    # used by this upstream source. Test logic and expectations are unchanged.
    math_source = src / 'external/vulkancts/modules/vulkan/renderpass/vktDynamicRenderingLocalReadTests.cpp'
    text = math_source.read_text()
    if '#include <cmath>' not in text:
        marker = '#include "deRandom.hpp"'
        if marker not in text:
            raise SystemExit('Unexpected CTS source for the macOS cmath fix')
        math_source.write_text(text.replace(marker, '#include <cmath>\n\n' + marker, 1))
    cmake = shutil.which('cmake') or str(ROOT / 'build/tools/python/cmake/data/bin/cmake')
    llvm = subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()
    sdk = subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()
    build = BASE / 'build'
    subprocess.run([cmake, '-S', str(src), '-B', str(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DDEQP_TARGET=osx', '-DSELECTED_BUILD_TARGETS=deqp-vk',
        '-DCMAKE_C_COMPILER=' + llvm + '/bin/clang', '-DCMAKE_CXX_COMPILER=' + llvm + '/bin/clang++',
        '-DCMAKE_OSX_SYSROOT=' + sdk, '-DPYTHON_EXECUTABLE=' + sys.executable,
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DDEQP_DISABLE_VK_VIDEO_TESTS=ON'], check=True)
    if not args.configure_only:
        subprocess.run([cmake, '--build', str(build), '--target', 'deqp-vk', '-j' + str(args.jobs)], check=True)
        print('Official CTS: ' + str(build / 'external/vulkancts/modules/vulkan/deqp-vk'))


if __name__ == '__main__':
    main()

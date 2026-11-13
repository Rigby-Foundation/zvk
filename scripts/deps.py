#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Fetch pinned Khronos interface headers; no renderer or compiler sources."""
import hashlib
from pathlib import Path
import shutil
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT/'build/deps'
SOURCES = [
    ('vulkan-headers', 'KhronosGroup/Vulkan-Headers', 'v1.3.296',
     'e204e0b3c19f622d197df945737f5db913d6621830999b8578d34e80a7c90585'),
    ('spirv-headers', 'KhronosGroup/SPIRV-Headers', 'vulkan-sdk-1.3.296.0',
     '1423d58a1171611d5aba2bf6f8c69c72ef9c38a0aca12c3493e4fda64c9b2dc6'),
]
DEPS.mkdir(parents=True, exist_ok=True)
for name, repo, tag, digest in SOURCES:
    archive = DEPS/(name+'.tar.gz')
    if not archive.exists():
        url = 'https://codeload.github.com/'+repo+'/tar.gz/refs/tags/'+tag
        print('Fetching '+url, flush=True)
        with urllib.request.urlopen(url, timeout=60) as response, archive.open('wb') as out:
            shutil.copyfileobj(response, out)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise SystemExit('Checksum mismatch: '+str(archive))
    target = DEPS/name
    if not target.exists():
        with tarfile.open(archive) as tar:
            prefix = tar.getmembers()[0].name.split('/')[0]
            tar.extractall(DEPS, filter='data')
        (DEPS/prefix).rename(target)
(DEPS/'.ready').touch()

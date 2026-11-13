#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Preserve copy2 semantics while sharing large APFS build-file extents."""
from pathlib import Path
import shutil
import subprocess
import sys


def copy2(source, target):
    source_path = Path(source)
    if (sys.platform == 'darwin' and not source_path.is_symlink()
            and source_path.stat().st_size >= 128 * 1024):
        result = subprocess.run(['cp', '-c', '-p', str(source), str(target)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode == 0:
            return target
    return shutil.copy2(source, target)

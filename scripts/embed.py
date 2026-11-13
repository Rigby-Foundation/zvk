#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Embed a shader helper without changing its source text."""
import json
from pathlib import Path
import sys

source, target, symbol = sys.argv[1:]
Path(target).write_text('/* Generated; edit ' + source + '. */\nstatic const char ' + symbol + '[] =\n' +
                       '\n'.join(json.dumps(line) for line in Path(source).read_text().splitlines(True)) + ';\n')

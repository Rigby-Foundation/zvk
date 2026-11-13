#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Write a development ICD manifest; it is never installed system-wide."""
import json,sys
from pathlib import Path
library,target=map(Path,sys.argv[1:])
target.write_text(json.dumps({'file_format_version':'1.0.0','ICD':{'library_path':str(library.resolve()),'api_version':'1.0.0'}},indent=2)+'\n')

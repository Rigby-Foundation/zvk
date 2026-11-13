#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate instruction size checks from the pinned Khronos SPIR-V grammar."""
import json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
grammar=json.loads((root/'build/deps/spirv-headers/include/spirv/unified1/spirv.core.grammar.json').read_text())
kinds={k['kind']:k for k in grammar['operand_kinds']}
def words(kind):
    return sum(words(k) for k in kinds[kind]['bases']) if 'bases' in kinds[kind] else 1
entries={}
for ins in grammar['instructions']:
    entries[ins['opcode']]=1+sum(words(o['kind']) for o in ins.get('operands',[]) if 'quantifier' not in o)
text='/* Generated from Khronos SPIR-V grammar. */\nstatic const unsigned char spv_min_words[] = {\n'
text+=''.join('    [%d] = %d,\n'%(op,n) for op,n in sorted(entries.items()))+'};\n'
(root/'build/spv_min_words.h').write_text(text)

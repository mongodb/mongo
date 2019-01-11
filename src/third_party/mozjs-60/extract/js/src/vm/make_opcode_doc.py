#!/usr/bin/python -B

""" Usage: make_opcode_doc.py PATH_TO_MOZILLA_CENTRAL

    This script generates SpiderMonkey bytecode documentation
    from js/src/vm/Opcodes.h.

    Output is written to stdout and should be pasted into the following
    MDN page:
    https://developer.mozilla.org/en-US/docs/SpiderMonkey/Internals/Bytecode
"""

from __future__ import print_function
import re
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import opcode

from xml.sax.saxutils import escape

SOURCE_BASE = 'http://dxr.mozilla.org/mozilla-central/source'

def override(value, override_value):
    if override_value != '':
        return override_value

    return value

def format_flags(flags):
    flags = filter(lambda x: x != 'JOF_BYTE', flags)
    if len(flags) == 0:
        return ''

    flags = map(lambda x: x.replace('JOF_', ''), flags)
    return ' ({flags})'.format(flags=', '.join(flags))

def print_opcode(opcode):
    names_template = '{name} [-{nuses}, +{ndefs}]{flags}'
    opcodes = sorted([opcode] + opcode.group,
                     key=lambda opcode: opcode.name)
    names = map(lambda code: names_template.format(name=escape(code.name),
                                                   nuses=override(code.nuses,
                                                                  opcode.nuses_override),
                                                   ndefs=override(code.ndefs,
                                                                  opcode.ndefs_override),
                                                   flags=format_flags(code.flags)),
                opcodes)
    if len(opcodes) == 1:
        values = ['{value} (0x{value:02x})'.format(value=opcode.value)]
    else:
        values_template = '{name}: {value} (0x{value:02x})'
        values = map(lambda code: values_template.format(name=escape(code.name),
                                                         value=code.value),
                    opcodes)

    print("""<dt id="{id}">{names}</dt>
<dd>
<table class="standard-table">
<tbody>
<tr><th>Value</th><td><code>{values}</code></td></tr>
<tr><th>Operands</th><td><code>{operands}</code></td></tr>
<tr><th>Length</th><td><code>{length}</code></td></tr>
<tr><th>Stack Uses</th><td><code>{stack_uses}</code></td></tr>
<tr><th>Stack Defs</th><td><code>{stack_defs}</code></td></tr>
</tbody>
</table>

{desc}
</dd>
""".format(id=opcodes[0].name,
           names='<br>'.join(names),
           values='<br>'.join(values),
           operands=escape(opcode.operands) or "&nbsp;",
           length=escape(override(opcode.length,
                                  opcode.length_override)),
           stack_uses=escape(opcode.stack_uses) or "&nbsp;",
           stack_defs=escape(opcode.stack_defs) or "&nbsp;",
           desc=opcode.desc)) # desc is already escaped

id_cache = dict()
id_count = dict()

def make_element_id(category, type=''):
    key = '{}:{}'.format(category, type)
    if key in id_cache:
        return id_cache[key]

    if type == '':
        id = category.replace(' ', '_')
    else:
        id = type.replace(' ', '_')

    if id in id_count:
        id_count[id] += 1
        id = '{}_{}'.format(id, id_count[id])
    else:
        id_count[id] = 1

    id_cache[key] = id
    return id

def print_doc(index):
    print("""<div>{{{{SpiderMonkeySidebar("Internals")}}}}</div>

<h2 id="Bytecode_Listing">Bytecode Listing</h2>

<p>This document is automatically generated from
<a href="{source_base}/js/src/vm/Opcodes.h">Opcodes.h</a> by
<a href="{source_base}/js/src/vm/make_opcode_doc.py">make_opcode_doc.py</a>.</p>
""".format(source_base=SOURCE_BASE))

    for (category_name, types) in index:
        print('<h3 id="{id}">{name}</h3>'.format(name=category_name,
                                                 id=make_element_id(category_name)))
        for (type_name, opcodes) in types:
            if type_name:
                print('<h4 id="{id}">{name}</h4>'.format(name=type_name,
                                                         id=make_element_id(category_name, type_name)))
            print('<dl>')
            for opcode in sorted(opcodes,
                                 key=lambda opcode: opcode.sort_key):
                print_opcode(opcode)
            print('</dl>')

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: make_opcode_doc.py PATH_TO_MOZILLA_CENTRAL",
              file=sys.stderr)
        sys.exit(1)
    dir = sys.argv[1]

    try:
        index, _ = opcode.get_opcodes(dir)
    except Exception as e:
        print("Error: {}".format(e.args[0]), file=sys.stderr)
        sys.exit(1)

    print_doc(index)

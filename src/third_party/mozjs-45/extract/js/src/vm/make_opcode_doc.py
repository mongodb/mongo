#!/usr/bin/python -B

""" Usage: make_opcode_doc.py PATH_TO_MOZILLA_CENTRAL

    This script generates SpiderMonkey bytecode documentation
    from js/src/vm/Opcodes.h and js/src/vm/Xdr.h.

    Output is written to stdout and should be pasted into the following
    MDN page:
    https://developer.mozilla.org/en-US/docs/SpiderMonkey/Internals/Bytecode
"""

from __future__ import print_function
import re
import sys
from xml.sax.saxutils import escape

SOURCE_BASE = 'http://mxr.mozilla.org/mozilla-central/source'

def error(message):
    print("Error: {message}".format(message=message), file=sys.stderr)
    sys.exit(1)

def get_xdr_version(dir):
    subtrahend_pat = re.compile('XDR_BYTECODE_VERSION_SUBTRAHEND\s*=\s*(\d+);', re.S)
    version_expr_pat = re.compile('XDR_BYTECODE_VERSION\s*=\s*uint32_t\(0xb973c0de\s*-\s*(.+?)\);', re.S)

    with open('{dir}/js/src/vm/Xdr.h'.format(dir=dir), 'r') as f:
        data = f.read()

    m = subtrahend_pat.search(data)
    if not m:
        error('XDR_BYTECODE_VERSION_SUBTRAHEND is not recognized.')

    subtrahend = int(m.group(1))

    m = version_expr_pat.search(data)
    if not m:
        error('XDR_BYTECODE_VERSION is not recognized.')

    return subtrahend

quoted_pat = re.compile(r"([^A-Za-z0-9]|^)'([^']+)'")
js_pat = re.compile(r"([^A-Za-z0-9]|^)(JS[A-Z0-9_\*]+)")
def codify(text):
    text = re.sub(quoted_pat, '\\1<code>\\2</code>', text)
    text = re.sub(js_pat, '\\1<code>\\2</code>', text)

    return text

space_star_space_pat = re.compile('^\s*\* ?', re.M)
def get_comment_body(comment):
    return re.sub(space_star_space_pat, '', comment).split('\n')

def parse_index(comment):
    index = []
    current_types = None
    category_name = ''
    category_pat = re.compile('\[([^\]]+)\]')
    for line in get_comment_body(comment):
        m = category_pat.search(line)
        if m:
            category_name = m.group(1)
            if category_name == 'Index':
                continue
            current_types = []
            index.append((category_name, current_types))
        else:
            type_name = line.strip()
            if type_name and current_types is not None:
                current_types.append((type_name, []))

    return index

class OpcodeInfo:
    def __init__(self):
        self.name = ''
        self.value = ''
        self.length = ''
        self.length_override = ''
        self.nuses = ''
        self.nuses_override = ''
        self.ndefs = ''
        self.ndefs_override = ''
        self.flags = ''
        self.operands = ''
        self.stack_uses = ''
        self.stack_defs = ''

        self.desc = ''

        self.category_name = ''
        self.type_name = ''

        self.group = []
        self.sort_key = ''

def find_by_name(list, name):
    for (n, body) in list:
        if n == name:
            return body

    return None

def add_to_index(index, opcode):
    types = find_by_name(index, opcode.category_name)
    if types is None:
        error("Category is not listed in index: "
              "{name}".format(name=opcode.category_name))
    opcodes = find_by_name(types, opcode.type_name)
    if opcodes is None:
        if opcode.type_name:
            error("Type is not listed in {category}: "
                  "{name}".format(category=opcode.category_name,
                                  name=opcode.type_name))
        types.append((opcode.type_name, [opcode]))
        return

    opcodes.append(opcode)

def format_desc(descs):
    current_type = ''
    desc = ''
    for (type, line) in descs:
        if type != current_type:
            if current_type:
                desc += '</{name}>\n'.format(name=current_type)
            current_type = type
            if type:
                desc += '<{name}>'.format(name=current_type)
        if current_type:
            desc += line + "\n"
    if current_type:
        desc += '</{name}>'.format(name=current_type)

    return desc

tag_pat = re.compile('^\s*[A-Za-z]+:\s*|\s*$')
def get_tag_value(line):
    return re.sub(tag_pat, '', line)

def get_opcodes(dir):
    iter_pat = re.compile(r"/\*(.*?)\*/"  # either a documentation comment...
                          r"|"
                          r"macro\("      # or a macro(...) call
                                 r"([^,]+),\s*"     # op
                                 r"([0-9]+),\s*"    # val
                                 r"[^,]+,\s*"       # name
                                 r"[^,]+,\s*"       # image
                                 r"([0-9\-]+),\s*"  # length
                                 r"([0-9\-]+),\s*"  # nuses
                                 r"([0-9\-]+),\s*"  # ndefs
                                 r"([^\)]+)"        # format
                          r"\)", re.S)
    stack_pat = re.compile('^(.*?)\s*=>\s*(.*?)$')

    index = []

    opcode = OpcodeInfo()
    merged = opcode

    with open('{dir}/js/src/vm/Opcodes.h'.format(dir=dir), 'r') as f:
        data = f.read()

    for m in re.finditer(iter_pat, data):
        comment = m.group(1)
        name = m.group(2)

        if comment:
            if '[Index]' in comment:
                index = parse_index(comment)
                continue

            if 'Operands:' not in comment:
                continue

            state = 'desc'
            stack = ''
            descs = []

            for line in get_comment_body(comment):
                if line.startswith('  Category:'):
                    state = 'category'
                    opcode.category_name = get_tag_value(line)
                elif line.startswith('  Type:'):
                    state = 'type'
                    opcode.type_name = get_tag_value(line)
                elif line.startswith('  Operands:'):
                    state = 'operands'
                    opcode.operands = get_tag_value(line)
                elif line.startswith('  Stack:'):
                    state = 'stack'
                    stack = get_tag_value(line)
                elif line.startswith('  len:'):
                    state = 'len'
                    opcode.length_override = get_tag_value(line)
                elif line.startswith('  nuses:'):
                    state = 'nuses'
                    opcode.nuses_override = get_tag_value(line)
                elif line.startswith('  ndefs:'):
                    state = 'ndefs'
                    opcode.ndefs_override = get_tag_value(line)
                elif state == 'desc':
                    if line.startswith(' '):
                        descs.append(('pre', escape(line[1:])))
                    else:
                        line = line.strip()
                        if line == '':
                            descs.append(('', line))
                        else:
                            descs.append(('p', codify(escape(line))))
                elif line.startswith('  '):
                    if state == 'operands':
                        opcode.operands += line.strip()
                    elif state == 'stack':
                        stack += line.strip()
                    elif state == 'len':
                        opcode.length_override += line.strip()
                    elif state == 'nuses':
                        opcode.nuses_override += line.strip()
                    elif state == 'ndefs':
                        opcode.ndefs_override += line.strip()

            opcode.desc = format_desc(descs)

            m2 = stack_pat.search(stack)
            if m2:
                opcode.stack_uses = m2.group(1)
                opcode.stack_defs = m2.group(2)

            merged = opcode
        elif name and not name.startswith('JSOP_UNUSED'):
            opcode.name = name
            opcode.value = int(m.group(3))
            opcode.length = m.group(4)
            opcode.nuses = m.group(5)
            opcode.ndefs = m.group(6)

            flags = []
            for flag in m.group(7).split('|'):
                if flag != 'JOF_BYTE':
                    flags.append(flag.replace('JOF_', ''))
            opcode.flags = ', '.join(flags)

            if merged == opcode:
                opcode.sort_key = opcode.name
                if opcode.category_name == '':
                    error("Category is not specified for "
                          "{name}".format(name=opcode.name))
                add_to_index(index, opcode)
            else:
                if merged.length != opcode.length:
                    error("length should be same for merged section: "
                          "{value1}({name1}) != "
                          "{value2}({name2})".format(name1=merged.name,
                                                     value1=merged.length,
                                                     name2=opcode.name,
                                                     value2=opcode.length))
                if merged.nuses != opcode.nuses:
                    error("nuses should be same for merged section: "
                          "{value1}({name1}) != "
                          "{value2}({name2})".format(name1=merged.name,
                                                     value1=merged.nuses,
                                                     name2=opcode.name,
                                                     value2=opcode.nuses))
                if merged.ndefs != opcode.ndefs:
                    error("ndefs should be same for merged section: "
                          "{value1}({name1}) != "
                          "{value2}({name2})".format(name1=merged.name,
                                                     value1=merged.ndefs,
                                                     name2=opcode.name,
                                                     value2=opcode.ndefs))
                merged.group.append(opcode)
                if opcode.name < merged.name:
                    merged.sort_key = opcode.name

            opcode = OpcodeInfo()

    return index

def override(value, override_value):
    if override_value != '':
        return override_value

    return value

def format_flags(flags):
    if flags == '':
        return ''

    return ' ({flags})'.format(flags=flags)

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

def make_element_id(name):
    return name.replace(' ', '-')

def print_doc(version, index):
    print("""<div>{{{{SpiderMonkeySidebar("Internals")}}}}</div>

<h2 id="Bytecode_Listing">Bytecode Listing</h2>

<p>This document is automatically generated from
<a href="{source_base}/js/src/vm/Opcodes.h">Opcodes.h</a> and
<a href="{source_base}/js/src/vm/Xdr.h">Xdr.h</a> by
<a href="{source_base}/js/src/vm/make_opcode_doc.py">make_opcode_doc.py</a>.</p>

<p>Bytecode version: <code>{version}</code>
(<code>0x{actual_version:08x}</code>).</p>
""".format(source_base=SOURCE_BASE,
           version=version,
           actual_version=0xb973c0de - version))

    for (category_name, types) in index:
        print('<h3 id="{id}">{name}</h3>'.format(name=category_name,
                                                 id=make_element_id(category_name)))
        for (type_name, opcodes) in types:
            if type_name:
                print('<h4 id="{id}">{name}</h4>'.format(name=type_name,
                                                         id=make_element_id(type_name)))
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
    version = get_xdr_version(dir)
    index = get_opcodes(dir)
    print_doc(version, index)

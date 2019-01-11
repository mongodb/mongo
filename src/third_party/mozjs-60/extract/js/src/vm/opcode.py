#!/usr/bin/python -B

from __future__ import print_function
import re
import sys
from xml.sax.saxutils import escape

quoted_pat = re.compile(r"([^A-Za-z0-9]|^)'([^']+)'")
js_pat = re.compile(r"([^A-Za-z0-9]|^)(JS[A-Z0-9_\*]+)")
def codify(text):
    text = re.sub(quoted_pat, '\\1<code>\\2</code>', text)
    text = re.sub(js_pat, '\\1<code>\\2</code>', text)

    return text

space_star_space_pat = re.compile('^\s*\* ?', re.M)
def get_comment_body(comment):
    return re.sub(space_star_space_pat, '', comment).split('\n')

quote_pat = re.compile('"([^"]+)"')
str_pat = re.compile('js_([^_]+)_str')
def parse_name(s):
    m = quote_pat.search(s)
    if m:
        return m.group(1)
    m = str_pat.search(s)
    if m:
        return m.group(1)
    return s

csv_pat = re.compile(', *')
def parse_csv(s):
    a = csv_pat.split(s)
    if len(a) == 1 and a[0] == '':
        return []
    return a

def get_stack_count(stack):
    if stack == '':
        return 0
    if '...' in stack:
        return -1
    return len(stack.split(','))

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

# Holds the information stored in the comment with the following format:
#   /*
#    * {desc}
#    *   Category: {category_name}
#    *   Type: {type_name}
#    *   Operands: {operands}
#    *   Stack: {stack_uses} => {stack_defs}
#    *   length: {length_override}
#    *   nuses: {nuses_override}
#    *   ndefs: {ndefs_override}
#    */
class CommentInfo:
    def __init__(self):
        self.desc = ''
        self.category_name = ''
        self.type_name = ''
        self.operands = ''
        self.stack_uses = ''
        self.stack_defs = ''
        self.length_override = ''
        self.nuses_override = ''
        self.ndefs_override = ''

# Holds the information stored in the macro with the following format:
#   macro({name}, {value}, {display_name}, {image}, {length}, {nuses}, {ndefs},
#         {flags})
# and the information from CommentInfo.
class OpcodeInfo:
    def __init__(self, comment_info):
        self.name = ''
        self.value = ''
        self.display_name = ''
        self.image = ''
        self.length = ''
        self.nuses = ''
        self.ndefs = ''
        self.flags = ''

        self.operands_array = []
        self.stack_uses_array = []
        self.stack_defs_array = []

        self.desc = comment_info.desc
        self.category_name = comment_info.category_name
        self.type_name = comment_info.type_name
        self.operands = comment_info.operands
        self.operands_array = comment_info.operands_array
        self.stack_uses = comment_info.stack_uses
        self.stack_uses_array = comment_info.stack_uses_array
        self.stack_defs = comment_info.stack_defs
        self.stack_defs_array = comment_info.stack_defs_array
        self.length_override = comment_info.length_override
        self.nuses_override = comment_info.nuses_override
        self.ndefs_override = comment_info.ndefs_override

        # List of OpcodeInfo that corresponds to macros after this.
        #   /*
        #    * comment
        #    */
        #   macro(JSOP_SUB, ...)
        #   macro(JSOP_MUL, ...)
        #   macro(JSOP_DIV, ...)
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
        raise Exception('Category is not listed in index: '
                        '{name}'.format(name=opcode.category_name))
    opcodes = find_by_name(types, opcode.type_name)
    if opcodes is None:
        if opcode.type_name:
            raise Exception('Type is not listed in {category}: '
                            '{name}'.format(category=opcode.category_name,
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
            desc += line + '\n'
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
                                 r"(?P<name>[^,]+),\s*"
                                 r"(?P<value>[0-9]+),\s*"
                                 r"(?P<display_name>[^,]+,)\s*"
                                 r"(?P<image>[^,]+),\s*"
                                 r"(?P<length>[0-9\-]+),\s*"
                                 r"(?P<nuses>[0-9\-]+),\s*"
                                 r"(?P<ndefs>[0-9\-]+),\s*"
                                 r"(?P<flags>[^\)]+)"
                          r"\)", re.S)
    stack_pat = re.compile(r"^(?P<uses>.*?)"
                           r"\s*=>\s*"
                           r"(?P<defs>.*?)$")

    opcodes = dict()
    index = []

    with open('{dir}/js/src/vm/Opcodes.h'.format(dir=dir), 'r') as f:
        data = f.read()

    comment_info = None
    opcode = None

    # The first opcode after the comment.
    group_head = None

    for m in re.finditer(iter_pat, data):
        comment = m.group(1)
        name = m.group('name')

        if comment:
            if '[Index]' in comment:
                index = parse_index(comment)
                continue

            if 'Operands:' not in comment:
                continue

            group_head = None

            comment_info = CommentInfo()

            state = 'desc'
            stack = ''
            descs = []

            for line in get_comment_body(comment):
                if line.startswith('  Category:'):
                    state = 'category'
                    comment_info.category_name = get_tag_value(line)
                elif line.startswith('  Type:'):
                    state = 'type'
                    comment_info.type_name = get_tag_value(line)
                elif line.startswith('  Operands:'):
                    state = 'operands'
                    comment_info.operands = get_tag_value(line)
                elif line.startswith('  Stack:'):
                    state = 'stack'
                    stack = get_tag_value(line)
                elif line.startswith('  len:'):
                    state = 'len'
                    comment_info.length_override = get_tag_value(line)
                elif line.startswith('  nuses:'):
                    state = 'nuses'
                    comment_info.nuses_override = get_tag_value(line)
                elif line.startswith('  ndefs:'):
                    state = 'ndefs'
                    comment_info.ndefs_override = get_tag_value(line)
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
                        comment_info.operands += line.strip()
                    elif state == 'stack':
                        stack += line.strip()
                    elif state == 'len':
                        comment_info.length_override += line.strip()
                    elif state == 'nuses':
                        comment_info.nuses_override += line.strip()
                    elif state == 'ndefs':
                        comment_info.ndefs_override += line.strip()

            comment_info.desc = format_desc(descs)

            comment_info.operands_array = parse_csv(comment_info.operands)
            comment_info.stack_uses_array = parse_csv(comment_info.stack_uses)
            comment_info.stack_defs_array = parse_csv(comment_info.stack_defs)

            m2 = stack_pat.search(stack)
            if m2:
                comment_info.stack_uses = m2.group('uses')
                comment_info.stack_defs = m2.group('defs')
        elif name and not name.startswith('JSOP_UNUSED'):
            opcode = OpcodeInfo(comment_info)

            opcode.name = name
            opcode.value = int(m.group('value'))
            opcode.display_name = parse_name(m.group('display_name'))
            opcode.image = parse_name(m.group('image'))
            opcode.length = m.group('length')
            opcode.nuses = m.group('nuses')
            opcode.ndefs = m.group('ndefs')
            opcode.flags = m.group('flags').split('|')

            if not group_head:
                group_head = opcode

                opcode.sort_key = opcode.name
                if opcode.category_name == '':
                    raise Exception('Category is not specified for '
                                    '{name}'.format(name=opcode.name))
                add_to_index(index, opcode)
            else:
                if group_head.length != opcode.length:
                    raise Exception('length should be same for opcodes of the'
                                    ' same group: '
                                    '{value1}({name1}) != '
                                    '{value2}({name2})'.format(
                                        name1=group_head.name,
                                        value1=group_head.length,
                                        name2=opcode.name,
                                        value2=opcode.length))
                if group_head.nuses != opcode.nuses:
                    raise Exception('nuses should be same for opcodes of the'
                                    ' same group: '
                                    '{value1}({name1}) != '
                                    '{value2}({name2})'.format(
                                        name1=group_head.name,
                                        value1=group_head.nuses,
                                        name2=opcode.name,
                                        value2=opcode.nuses))
                if group_head.ndefs != opcode.ndefs:
                    raise Exception('ndefs should be same for opcodes of the'
                                    ' same group: '
                                    '{value1}({name1}) != '
                                    '{value2}({name2})'.format(
                                        name1=group_head.name,
                                        value1=group_head.ndefs,
                                        name2=opcode.name,
                                        value2=opcode.ndefs))

                group_head.group.append(opcode)

                if opcode.name < group_head.name:
                    group_head.sort_key = opcode.name

            opcodes[name] = opcode

            # Verify stack notation.
            nuses = int(opcode.nuses)
            ndefs = int(opcode.ndefs)

            stack_nuses = get_stack_count(opcode.stack_uses)
            stack_ndefs = get_stack_count(opcode.stack_defs)

            if nuses != -1 and stack_nuses != -1 and nuses != stack_nuses:
                raise Exception('nuses should match stack notation: {name}: '
                                '{nuses} != {stack_nuses} '
                                '(stack_nuses)'.format(
                                    name=name,
                                    nuses=nuses,
                                    stack_nuses=stack_nuses,
                                    stack_uses=opcode.stack_uses))
            if ndefs != -1 and stack_ndefs != -1 and ndefs != stack_ndefs:
                raise Exception('ndefs should match stack notation: {name}: '
                                '{ndefs} != {stack_ndefs} '
                                '(stack_ndefs)'.format(
                                    name=name,
                                    ndefs=ndefs,
                                    stack_ndefs=stack_ndefs,
                                    stack_defs=opcode.stack_defs))

    return index, opcodes

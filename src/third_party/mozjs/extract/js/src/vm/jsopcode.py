#!/usr/bin/env python3 -B
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

import re

quoted_pat = re.compile(r"([^A-Za-z0-9]|^)'([^']+)'")
js_pat = re.compile(r"([^A-Za-z0-9]|^)(JS[A-Z0-9_\*]+)")


def codify(text):
    text = re.sub(quoted_pat, "\\1<code>\\2</code>", text)
    text = re.sub(js_pat, "\\1<code>\\2</code>", text)

    return text


space_star_space_pat = re.compile("^\s*\* ?", re.M)


def get_comment_body(comment):
    return re.sub(space_star_space_pat, "", comment).split("\n")


quote_pat = re.compile('"([^"]+)"')
str_pat = re.compile("js_([^_]+)_str")


def parse_name(s):
    m = quote_pat.search(s)
    if m:
        return m.group(1)
    m = str_pat.search(s)
    if m:
        return m.group(1)
    return s


csv_pat = re.compile(", *")


def parse_csv(s):
    a = csv_pat.split(s)
    if len(a) == 1 and a[0] == "":
        return []
    return a


def get_stack_count(stack):
    if stack == "":
        return 0
    if "..." in stack:
        return -1
    return len(stack.split(","))


def parse_index(comment):
    index = []
    current_types = None
    category_name = ""
    category_pat = re.compile("\[([^\]]+)\]")
    for line in get_comment_body(comment):
        m = category_pat.search(line)
        if m:
            category_name = m.group(1)
            if category_name == "Index":
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
#    */


class CommentInfo:
    def __init__(self):
        self.desc = ""
        self.category_name = ""
        self.type_name = ""
        self.operands = ""
        self.stack_uses = ""
        self.stack_defs = ""


# Holds the information stored in the macro with the following format:
#   MACRO({op}, {op_snake}, {token}, {length}, {nuses}, {ndefs}, {format})
# and the information from CommentInfo.


class OpcodeInfo:
    def __init__(self, value, comment_info):
        self.op = ""
        self.op_snake = ""
        self.value = value
        self.token = ""
        self.length = ""
        self.nuses = ""
        self.ndefs = ""
        self.format_ = ""

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

        # List of OpcodeInfo that corresponds to macros after this.
        #   /*
        #    * comment
        #    */
        #   MACRO(JSOP_SUB, ...)
        #   MACRO(JSOP_MUL, ...)
        #   MACRO(JSOP_DIV, ...)
        self.group = []

        self.sort_key = ""


def find_by_name(list, name):
    for (n, body) in list:
        if n == name:
            return body

    return None


def add_to_index(index, opcode):
    types = find_by_name(index, opcode.category_name)
    if types is None:
        raise Exception(
            "Category is not listed in index: "
            "{name}".format(name=opcode.category_name)
        )
    opcodes = find_by_name(types, opcode.type_name)
    if opcodes is None:
        if opcode.type_name:
            raise Exception(
                "Type is not listed in {category}: "
                "{name}".format(category=opcode.category_name, name=opcode.type_name)
            )
        types.append((opcode.type_name, [opcode]))
        return

    opcodes.append(opcode)


tag_pat = re.compile("^\s*[A-Za-z]+:\s*|\s*$")


def get_tag_value(line):
    return re.sub(tag_pat, "", line)


RUST_OR_CPP_KEYWORDS = {
    "and",
    "case",
    "default",
    "double",
    "false",
    "goto",
    "in",
    "new",
    "not",
    "or",
    "return",
    "throw",
    "true",
    "try",
    "typeof",
    "void",
}


def get_opcodes(dir):
    iter_pat = re.compile(
        r"/\*(.*?)\*/"  # either a documentation comment...
        r"|"
        r"MACRO\("  # or a MACRO(...) call
        r"(?P<op>[^,]+),\s*"
        r"(?P<op_snake>[^,]+),\s*"
        r"(?P<token>[^,]+,)\s*"
        r"(?P<length>[0-9\-]+),\s*"
        r"(?P<nuses>[0-9\-]+),\s*"
        r"(?P<ndefs>[0-9\-]+),\s*"
        r"(?P<format>[^\)]+)"
        r"\)",
        re.S,
    )
    stack_pat = re.compile(r"^(?P<uses>.*?)" r"\s*=>\s*" r"(?P<defs>.*?)$")

    opcodes = dict()
    index = []

    with open("{dir}/js/src/vm/Opcodes.h".format(dir=dir), "r", encoding="utf-8") as f:
        data = f.read()

    comment_info = None
    opcode = None

    # The first opcode after the comment.
    group_head = None
    next_opcode_value = 0

    for m in re.finditer(iter_pat, data):
        comment = m.group(1)
        op = m.group("op")

        if comment:
            if "[Index]" in comment:
                index = parse_index(comment)
                continue

            if "Operands:" not in comment:
                continue

            group_head = None

            comment_info = CommentInfo()

            state = "desc"
            stack = ""
            desc = ""

            for line in get_comment_body(comment):
                if line.startswith("  Category:"):
                    state = "category"
                    comment_info.category_name = get_tag_value(line)
                elif line.startswith("  Type:"):
                    state = "type"
                    comment_info.type_name = get_tag_value(line)
                elif line.startswith("  Operands:"):
                    state = "operands"
                    comment_info.operands = get_tag_value(line)
                elif line.startswith("  Stack:"):
                    state = "stack"
                    stack = get_tag_value(line)
                elif state == "desc":
                    desc += line + "\n"
                elif line.startswith("   "):
                    if line.isspace():
                        pass
                    elif state == "operands":
                        comment_info.operands += " " + line.strip()
                    elif state == "stack":
                        stack += " " + line.strip()
                else:
                    raise ValueError(
                        "unrecognized line in comment: {!r}\n\nfull comment was:\n{}".format(
                            line, comment
                        )
                    )

            comment_info.desc = desc

            comment_info.operands_array = parse_csv(comment_info.operands)
            comment_info.stack_uses_array = parse_csv(comment_info.stack_uses)
            comment_info.stack_defs_array = parse_csv(comment_info.stack_defs)

            m2 = stack_pat.search(stack)
            if m2:
                comment_info.stack_uses = m2.group("uses")
                comment_info.stack_defs = m2.group("defs")
        else:
            assert op is not None
            opcode = OpcodeInfo(next_opcode_value, comment_info)
            next_opcode_value += 1

            opcode.op = op
            opcode.op_snake = m.group("op_snake")
            opcode.token = parse_name(m.group("token"))
            opcode.length = m.group("length")
            opcode.nuses = m.group("nuses")
            opcode.ndefs = m.group("ndefs")
            opcode.format_ = m.group("format").split("|")

            expected_snake = re.sub(r"(?<!^)(?=[A-Z])", "_", opcode.op).lower()
            if expected_snake in RUST_OR_CPP_KEYWORDS:
                expected_snake += "_"
            if opcode.op_snake != expected_snake:
                raise ValueError(
                    "Unexpected snake-case name for {}: expected {!r}, got {!r}".format(
                        opcode.op_camel, expected_snake, opcode.op_snake
                    )
                )

            if not group_head:
                group_head = opcode

                opcode.sort_key = opcode.op
                if opcode.category_name == "":
                    raise Exception(
                        "Category is not specified for " "{op}".format(op=opcode.op)
                    )
                add_to_index(index, opcode)
            else:
                if group_head.length != opcode.length:
                    raise Exception(
                        "length should be same for opcodes of the"
                        " same group: "
                        "{value1}({op1}) != "
                        "{value2}({op2})".format(
                            op1=group_head.op,
                            value1=group_head.length,
                            op2=opcode.op,
                            value2=opcode.length,
                        )
                    )
                if group_head.nuses != opcode.nuses:
                    raise Exception(
                        "nuses should be same for opcodes of the"
                        " same group: "
                        "{value1}({op1}) != "
                        "{value2}({op2})".format(
                            op1=group_head.op,
                            value1=group_head.nuses,
                            op2=opcode.op,
                            value2=opcode.nuses,
                        )
                    )
                if group_head.ndefs != opcode.ndefs:
                    raise Exception(
                        "ndefs should be same for opcodes of the"
                        " same group: "
                        "{value1}({op1}) != "
                        "{value2}({op2})".format(
                            op1=group_head.op,
                            value1=group_head.ndefs,
                            op2=opcode.op,
                            value2=opcode.ndefs,
                        )
                    )

                group_head.group.append(opcode)

                if opcode.op < group_head.op:
                    group_head.sort_key = opcode.op

            opcodes[op] = opcode

            # Verify stack notation.
            nuses = int(opcode.nuses)
            ndefs = int(opcode.ndefs)

            stack_nuses = get_stack_count(opcode.stack_uses)
            stack_ndefs = get_stack_count(opcode.stack_defs)

            if nuses != -1 and stack_nuses != -1 and nuses != stack_nuses:
                raise Exception(
                    "nuses should match stack notation: {op}: "
                    "{nuses} != {stack_nuses} "
                    "(stack_nuses)".format(op=op, nuses=nuses, stack_nuses=stack_nuses)
                )
            if ndefs != -1 and stack_ndefs != -1 and ndefs != stack_ndefs:
                raise Exception(
                    "ndefs should match stack notation: {op}: "
                    "{ndefs} != {stack_ndefs} "
                    "(stack_ndefs)".format(op=op, ndefs=ndefs, stack_ndefs=stack_ndefs)
                )

    return index, opcodes

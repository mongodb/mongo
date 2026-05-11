#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

# Examine WT_WITH_* macros and complain if an early exit out of the
# macro is done.  These macros potentially restore state after they execute
# an expression, and an early exit, like a return/goto means the state
# will not be restored.
#
# Files appear verbatim, separated by a recognizable pattern.
import re, sys

def get_indent(line):
    return len(line) - len(line.lstrip())

filepat = re.compile(r'===(.*)===')
withpat = re.compile(r'WT_WITH_')
exitpat = re.compile(r'(WT_ERR|WT_RET|return\(|goto )')
filename = ''
linenum = 0
with_indent = -1
with_linenum = -1
with_line = None
for line in sys.stdin:
    m = filepat.search(line)
    if m != None:
        filename = m.group(1)
        linenum = 0
        with_indent = -1
        with_linenum = -1
        with_line = None
    linenum += 1

    # Are we within a WT_WITH?
    if with_indent >= 0:
        cur_indent = get_indent(line)
        # Have we "broken out" of the WT_WITH_.... macro?
        # Our enforced formatting ensures this happens when the indent finishes.
        if cur_indent <= with_indent:
            # Yes, broken out
            with_indent = -1
            with_linenum = -1
            with_line = None
        elif exitpat.search(line):
            print('{}: line {}: early exit: {}\n  from: {}: line {}: {}'.format(
                filename, linenum, line.strip(), filename, with_linenum,
                with_line.strip()))
    else:
        m = withpat.search(line)
        if m:
            with_indent = get_indent(line)
            with_linenum = linenum
            with_line = line

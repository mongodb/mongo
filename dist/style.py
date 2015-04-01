#!/usr/bin/env python

# Check the style of WiredTiger C code.
from dist import source_files
import re, sys

# Complain if a function comment is missing.
def missing_comment():
    for f in source_files():
        skip_re = re.compile(r'DO NOT EDIT: automatically built')
        func_re = re.compile(
            r'(/\*(?:[^\*]|\*[^/])*\*/)?\n\w[\w \*]+\n(\w+)', re.DOTALL)
        s = open(f, 'r').read()
        if skip_re.search(s):
            continue
        for m in func_re.finditer(s):
            if not m.group(1) or \
               not m.group(1).startswith('/*\n * %s --\n' % m.group(2)):
                   print "%s:%d: missing comment for %s" % \
                           (f, s[:m.start(2)].count('\n'), m.group(2))

# Display lines that could be joined.
def lines_could_join():
    skip_re = re.compile(r'__asm__')
    match_re = re.compile('(^[ \t].*\()\n^[ \t]*([^\n]*)', re.MULTILINE)
    for f in source_files():
        s = open(f, 'r').read()
        if skip_re.search(s):
            continue

        for m in match_re.finditer(s):
            if len(m.group(1).expandtabs()) + \
                len(m.group(2).expandtabs()) < 80:
                    print f + ': lines may be combined: '
                    print '\t' + m.group(1).lstrip() + m.group(2)
                    print


missing_comment()

# Don't display lines that could be joined by default; in some cases, the code
# isn't maintained by WiredTiger, or the line splitting enhances readability.
if len(sys.argv) > 1:
    lines_could_join()

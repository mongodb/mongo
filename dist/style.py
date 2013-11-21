#!/usr/bin/env python

# Check the style of WiredTiger C code.
from dist import source_files
import re

skip_re = re.compile(r'DO NOT EDIT: automatically built')
func_re = re.compile(r'(/\*(?:[^\*]|\*[^/])*\*/)?\n\w[\w ]+\n(\w+)', re.DOTALL)

for f in source_files():
    s = open(f, 'r').read()
    if skip_re.search(s):
        continue
    for m in func_re.finditer(s):
        if not m.group(1) or \
           not m.group(1).startswith('/*\n * %s --\n' % m.group(2)):
               print "%s:%d: missing comment for %s" % \
                       (f, s[:m.start(2)].count('\n'), m.group(2))

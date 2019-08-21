#!/usr/bin/env python

# Check the style of WiredTiger C code.
from __future__ import print_function
import re, sys
from dist import source_files

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
                len(m.group(2).expandtabs()) < 100:
                    print(f + ': lines may be combined: ')
                    print('\t' + m.group(1).lstrip() + m.group(2))
                    print()

# Don't display lines that could be joined by default; in some cases, the code
# isn't maintained by WiredTiger, or the line splitting enhances readability.
if len(sys.argv) > 1:
    lines_could_join()

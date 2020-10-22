#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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


# This Python script is run as part of generating the documentation for the
# WiredTiger reference manual.  It changes comments to Javadoc style
# (i.e., from "/*!" to "/**"), because the latter are configured to not
# search for brief descriptions at the beginning of pages.

import re, sys

progname = 'doxfilter.py'
linenum = 0
filename = '<unknown>'

def err(arg):
    sys.stderr.write(filename + ':' + str(linenum) + ': ERROR: ' + arg + '\n')
    sys.exit(1)

def process(source):
    return source.replace(r'/*!', r'/**')

if __name__ == '__main__':
    for f in sys.argv[1:]:
        filename = f
        with open(f, 'r') as infile:
            sys.stdout.write(process(infile.read()))
        sys.exit(0)

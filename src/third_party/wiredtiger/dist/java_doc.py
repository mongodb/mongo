#!/usr/bin/env python

# This program pulls the function names from wiredtiger.in and generates
# an input file for Java SWIG that adds doxygen copydoc comments to functions.

import os, re, sys
import api_data
from dist import compare_srcfile

# Temporary file.
tmp_file = '__tmp'

#####################################################################
# Update wiredtiger.in with doxygen comments
#####################################################################
f='../src/include/wiredtiger.in'
o='../lang/java/java_doc.i'
tfile = open(tmp_file, 'w')

tfile.write('''/* DO NOT EDIT: automatically built by dist/java_doc.py. */

''')

cclass_re = re.compile('^struct __([a-z_]*) {')
cfunc_re = re.compile('\t.*? __F\(([a-z_]*)\)')

curr_class = ""
for line in open(f, 'r'):

    m = cclass_re.match(line)
    if m:
        curr_class = m.group(1)

    if curr_class == "":
        continue

    m = cfunc_re.match(line)
    if m:
        tfile.write('COPYDOC(__' + curr_class.lower() + ', ' +
        curr_class.upper() + ', ' + m.group(1) + ')\n')

tfile.close()
compare_srcfile(tmp_file, o)

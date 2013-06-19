#!/usr/bin/env python

import os, re, sys, textwrap
import log_data
from dist import compare_srcfile

# Temporary file.
tmp_file = '__tmp'

# Map log record types to C
c_types = {
		'string' : 'const char *',
}

# Map log record types to format strings
fmt_types = {
		'string' : 'S',
}

#####################################################################
# Create log.i with inline functions for each log record type.
#####################################################################
f='../src/include/log.i'
tfile = open(tmp_file, 'w')

tfile.write('/* DO NOT EDIT: automatically built by dist/log.py. */\n')

tfile.close()
compare_srcfile(tmp_file, f)

#####################################################################
# Create log_desc.c with descriptors for each log record type.
#####################################################################
f='../src/log/log_desc.c'
tfile = open(tmp_file, 'w')

tfile.write('''/* DO NOT EDIT: automatically built by dist/log.py. */''')

tfile.close()
compare_srcfile(tmp_file, f)

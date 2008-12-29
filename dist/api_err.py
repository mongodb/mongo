# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Read the api_err file and output C #defines and associated error code.

import re
from dist import compare_srcfile

# Read the source file and build a list of items.
def err_build():
	l = []
	err_re = re.compile(r'\b(WT_([A-Z]|_)+)\t(.*)')
	for match in err_re.finditer(open('api_err', 'r').read()):
		l += [[match.group(1), match.group(3)]]
	return (l)

# Read the source file and build a list of items.
list = err_build()

# Update the #defines in the wiredtiger.in file.
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../inc_posix/wiredtiger.in', 'r'):
	if not skip:
		tfile.write(line)
	if line.count('Error return section: END'):
		tfile.write(line)
		skip = 0
	elif line.count('Error return section: BEGIN'):
		tfile.write(' */\n')
		skip = 1

		# We don't want our error returns to conflict with any other
		# package, so use an uncommon range, specifically, -31,800 to
		# -31,999.
		v = -31800
		for l in list:
			tfile.write('#define\t' + l[0] + '\t' + str(v) + '\n')
			v -= 1
		tfile.write('/*\n')
tfile.close()
compare_srcfile(tmp_file, '../inc_posix/wiredtiger.in')

# Output the wt_strerror code.
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/api_err.py. */\n\n')
tfile.write('#include "wt_internal.h"\n\n')
tfile.write('/*\n')
tfile.write(' * wt_strerror --\n')
tfile.write(' *\tReturn a string for any error value.\n')
tfile.write(' */\n')
tfile.write('char *\n')
tfile.write('wt_strerror(int error)\n')
tfile.write('{\n')
tfile.write('\tstatic char errbuf[64];\n')
tfile.write('\tchar *p;\n\n')
tfile.write('\tif (error == 0)\n')
tfile.write('\t\treturn ("Successful return: 0");\n\n')
tfile.write('\tswitch (error) {\n')

# We don't want our error returns to conflict with any other
# package, so use an uncommon range, specifically, -31,800 to
# -31,999.
v = -31800
for l in list:
	tfile.write('\tcase ' + l[0] + ':\n')
	tfile.write('\t\treturn ("' + l[0] + ': ' + l[1] + '");\n')
	v -= 1

tfile.write('\tdefault:\n')
tfile.write('\t\tif (error > 0 && (p = strerror(error)) != NULL)\n')
tfile.write('\t\t\treturn (p);\n')
tfile.write('\t\tbreak;\n')
tfile.write('\t}\n\n')
tfile.write('\t/*\n')
tfile.write('\t * !!!\n')
tfile.write('\t * Not thread-safe, but this is never supposed to happen.\n')
tfile.write('\t */\n')
tfile.write('\t(void)snprintf(errbuf, sizeof(errbuf), ' +\
    '"Unknown error: %d", error);\n')
tfile.write('\treturn (errbuf);\n')
tfile.write('}\n')
tfile.close()
compare_srcfile(tmp_file, '../support/strerror.c')

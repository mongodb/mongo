# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$
#
# Read the getset file and output C for the get/set pairs.

import re, string, sys

def output():
	# Output the getter
	s = 'void\n__wt_' + handle + '_get_' +\
	    list[0].split('\t')[0] +\
	    '(\n\t' +\
	    handle.upper() +\
	    ' *handle'
	argcnt = 1
	for l in list:
		s += ',\n\t' +\
		    l.split('\t')[1].replace('@S', '*store' + str(argcnt) + 'p')
		argcnt += 1
	s = s + ')\n{\n'
	argcnt = 1;
	for l in list:
		s += '\t*store' + str(argcnt) + 'p = ' +\
		    'handle->' + l.split('\t')[0] + ';\n'
		argcnt += 1
	s = s + '}\n'
	print s

	# Output the setter
	s = 'int\n__wt_' + handle + '_set_' +\
	    list[0].split('\t')[0] +\
	    '(\n\t' +\
	    handle.upper() +\
	    ' *handle'
	argcnt = 1
	for l in list:
		s += ',\n\t' +\
		    l.split('\t')[1].replace('@S', 'store' + str(argcnt))
		argcnt += 1
	s += ')\n{\n'
	argcnt = 1;

	# Verify means we call a standard verification routine because
	# there are constraints or side-effects on setting the value.
	if condition.count('verify'):
		s += '\tint ret;\n\n' +\
		    '\tif ((ret = __wt_' +\
		    handle +\
		    '_set_' +\
		    list[0].split('\t')[0] +\
		    '_verify(handle, '

		argcnt = 1
		for l in list:
			if argcnt > 1:
				s += ', '
			s += 'store' + str(argcnt)
			argcnt += 1
		s += ')) != 0)\n\t\treturn (ret);\n\n'


	argcnt = 1
	for l in list:
		s += '\thandle->' + l.split('\t')[0] +\
		     ' = store' + str(argcnt) + ';\n'
		argcnt += 1
	s += '\treturn (0);\n}\n'
	print s

print '/* DO NOT EDIT: automatically built by getset.py. */'
print '#include "wt_internal.h"'

setter_re = re.compile(r'^[a-z]')
field_re = re.compile(r'^\t[a-z]')
list = []
for line in open('getset', 'r'):
	if setter_re.match(line):
		if list:
			output()
			list=[]
		s = line.split('\t')
		handle = s[0]
		condition = s[1]
	elif field_re.match(line):
		list.append(string.strip(line))

if list:
	output()

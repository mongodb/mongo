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
	for l in list:
		s += ',\n\t' +\
		    l.split('\t')[1].replace('@S', '*' + l.split('\t')[0] + 'p')
	s = s + ')\n{\n'
	for l in list:
		s += '\t*' + l.split('\t')[0] + 'p = ' +\
		    'handle->' + l.split('\t')[0] + ';\n'
	s = s + '}\n'
	print s

	# Output the setter
	s = 'int\n__wt_' + handle + '_set_' +\
	    list[0].split('\t')[0] +\
	    '(\n\t' +\
	    handle.upper() +\
	    ' *handle'
	for l in list:
		s += ',\n\t' +\
		    l.split('\t')[1].replace('@S', l.split('\t')[0])
	s += ')\n{\n'

	# Verify needs a local return variable.
	if condition.count('verify'):
		s += '\tint ret;\n\n'

	# Default means any values that aren't set (that are set to 0)
	# should be loaded from the existing values before calling the
	# verification routine.
	#
	# XXX
	# This is ugly -- it supports Db.set_pagesize and nothing else,
	# we may need to re-think this in the future.
	if condition.count('default'):
		for l in list:
			s += '\tif (' + l.split('\t')[0] +\
			    ' == 0)\n\t\t' + l.split('\t')[0] +\
			    ' = handle->' + l.split('\t')[0] + ';\n'
		s += '\n'

	# Verify means we call a standard verification routine because
	# there are constraints or side-effects on setting the value.
	if condition.count('verify'):
		s += '\tif ((ret = __wt_' +\
		    handle +\
		    '_set_' +\
		    list[0].split('\t')[0] +\
		    '_verify(\n\t    handle'

		for l in list:
			s += ', ' + l.split('\t')[0]
		s += ')) != 0)\n\t\treturn (ret);\n\n'


	for l in list:
		s += '\thandle->' + l.split('\t')[0] +\
		     ' = ' + l.split('\t')[0] + ';\n'
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

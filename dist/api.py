# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Read the getset file and output C for the get/set pairs.
#
# The format of the getset file:
#	handle<tab>
#	field<tab> 
#	type (with @ for variable, and additional *)<tab>

import re, sys

print '/* DO NOT EDIT: automatically built by getset.py. */'
print '#include "wt_internal.h"'

getset_re = re.compile(r'^([a-z][^\t\n]+)\t*([^\t\n]+)\t*([^\t\n]+)')

# Read the api_defines file, building a list of flags for each method.
for line in open('getset', 'r'):
	if getset_re.match(line):
		handle = getset_re.match(line).group(1)
		field = getset_re.match(line).group(2)
		type = getset_re.match(line).group(3)
		type = type.replace('@H', handle.upper())

		print "void"
		print "__wt_%s_get_%s(%s *handle, %s)" % (\
		    handle, field, handle.upper(),\
		    type.replace('@S', '*storep'))
		print "{"
		print "\t*storep = handle->%s;" % field
		print "}"
		print "void"
		print "__wt_%s_set_%s(%s *handle, %s)" % (\
		    handle, field, handle.upper(),\
		    type.replace('@S', 'store'))
		print "{"
		print "\thandle->%s = store;" % field
		print "}"

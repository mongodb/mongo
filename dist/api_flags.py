# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Read the api_defines file and output a C header file using the minimum
# number of distinct bits to ensure flags don't collide.
#
# The format of the api_defines file:
#	Name (usually a method name)
#	<tab>	Flag

import re, sys

print '\n\n/* DO NOT EDIT: automatically built by api_defines.py. */'

# method_re matches a method name, that is, a line starting with alphanumeric
# characters, possibly including a '.'.
method_re = re.compile(r'^([\w.]+)')

# flag_re matches a flag, that is, a line starting with whitespace followed
# by some alphanumeric characters.
flag_re = re.compile(r'^\s+(\w+)')

flag_cnt = {}		# Dictionary [flag] : [reference count]
flag_methods = {}	# Dictionary [flag] : [method ...]
method_mask = {}	# Dictionary [method] : [used flag mask]
# Read the api_defines file, building a list of flags for each method.
for line in open('api_defines', 'r'):
	if method_re.match(line):
		method = method_re.match(line).group(1)
		method_mask[method] = 0x0
	elif flag_re.match(line):
		flag = flag_re.match(line).group(1)
		if flag not in flag_cnt:
			flag_cnt[flag] = 1
			flag_methods[flag] = []
		else:
			flag_cnt[flag] += 1
		flag_methods[flag].append(method)

# Create list of possible bit masks.
bits = [2 ** i for i in range(0, 32)]

# Walk the list of flags in reverse, sorted-by-reference count order.  For
# each flag, find a bit that's not currently in use by any method using the
# flag.
flag_bit = {}		# Dictionary [flag] : [bit value]
for f in sorted(flag_cnt.iteritems(),\
    key = lambda (k, v) : (v, k), reverse = True):
	mask = 0xffffffff
	for m in flag_methods[f[0]]:
		mask &= ~method_mask[m]
	if mask == 0:
		print >> sys.stderr,\
		    "api_defines: ran out of flags at %s method" % m
		sys.exit(1)
	for b in bits:
		if mask & b:
			mask = b
			break
	flag_bit[f[0]] = mask
	for m in flag_methods[f[0]]:
		method_mask[m] |= mask

# Print out the flag masks in hex.
#	Assumes tab stops set to 8 characters.
for f in sorted(flag_cnt.iteritems()):
	print "#define\tWT_%s%s%#010x" %\
	    (f[0], "\t" * max(1, 5 - len(f[0]) / 8), flag_bit[f[0]])

# Print out the API masks in hex.
#	Assumes tab stops set to 8 characters.
print ""
for f in sorted(method_mask.iteritems()):
	print "#define\tWT_APIMASK_%s%s%#010x" %\
	    (f[0].upper().replace('.', '_'),\
	    "\t" * max(1, 5 - len(f[0]) / 8), f[1])

# $Id$

# Read the api_flags file and output a C header file using the minimum
# number of distinct bits to ensure flags don't collide.
#
# The format of the api_flags file:
#	Name (usually a method name)
#	<tab>	Flag

import os, re, sys
from dist import compare_srcfile

# Load the flags dictionary.
import api_class
flags = api_class.flags

flag_cnt = {}		# Dictionary [flag] : [reference count]
flag_name = {}		# Dictionary [flag] : [name ...]
name_mask = {}		# Dictionary [name] : [used flag mask]

# Step through the flags dictionary and build our local dictionaries.
for method in flags.items():
	name_mask[method[0]] = 0x0
	for flag in method[1]:
		if flag == '__NONE__':
			continue
		if flag not in flag_cnt:
			flag_cnt[flag] = 1
			flag_name[flag] = []
		else:
			flag_cnt[flag] += 1
		flag_name[flag].append(method[0])

# Create list of possible bit masks.
bits = [2 ** i for i in range(0, 32)]

# Walk the list of flags in reverse, sorted-by-reference count order.  For
# each flag, find a bit that's not currently in use by any method using the
# flag.
flag_bit = {}		# Dictionary [flag] : [bit value]
for f in sorted(flag_cnt.items(),\
    key = lambda k_v : (k_v[1], k_v[0]), reverse = True):
	mask = 0xffffffff
	for m in flag_name[f[0]]:
		mask &= ~name_mask[m]
	if mask == 0:
		print >>sys.stder, "api_flags: ran out of flags at " + m + " method",
		sys.exit(1)
	for b in bits:
		if mask & b:
			mask = b
			break
	flag_bit[f[0]] = mask
	for m in flag_name[f[0]]:
		name_mask[m] |= mask

# Print out the flag masks in hex.
#	Assumes tab stops set to 8 characters.
flag_info = ''
for f in sorted(flag_cnt.items()):
	flag_info += "#define\tWT_%s%s%#010x\n" %\
	    (f[0],\
	    "\t" * max(1, 6 - int((len('WT_') + len(f[0])) / 8)),\
	    flag_bit[f[0]])

# Print out the API masks in hex.
#	Assumes tab stops set to 8 characters.
flag_info += '\n'
for f in sorted(name_mask.items()):
	# Only write out masks for API/method functions.
	flag_info += "#define\tWT_APIMASK_%s%s%#010x\n" %\
	    (f[0].upper().replace('.', '_'),\
	    "\t" * max(1, 6 - int((len('WT_APIMASK_') + len(f[0])) / 8)), f[1])

# Update the wiredtiger.in file with the flags information.
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/api_int.h', 'r'):
	if skip:
		if line.count('API flags section: END'):
			tfile.write('/*\n' + line)
			skip = 0
	else:
		tfile.write(line)
	if line.count('API flags section: BEGIN'):
		skip = 1
		tfile.write(' */\n')
		tfile.write(flag_info)
tfile.close()
compare_srcfile(tmp_file, '../src/include/api_int.h')

os.remove(tmp_file)

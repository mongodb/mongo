# See the file LICENSE for redistribution information.
#
# Copyright (c) 2009 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Read a memory.out file and output a list of memory errors.

import string, sys

# Read a memory.out file.  Enter each allocation into the dictionary,
# remove each from the dictionary.
entries = {}
for line in open('memory.out', 'r').readlines():
	op, addr = string.split(line)
	if op == "A":
		if entries.has_key(addr):
			print "address 0x%s repeatedly allocated" % addr
		else:
			entries[addr] = 1
	else:
		if entries.has_key(addr):
			del entries[addr]
		else:
			print "unallocated address 0x%s freed" % addr

# Print out any addresses never freed.
for entry in entries.keys():
	print "address 0x%s allocated but never freed" % entry

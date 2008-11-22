# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$
import re, sys

# source_files --
#	List the source files in filelist.
def source_files():
	file_re = re.compile(r'^\w(\w|/)*/((\w|.)*)')
	for line in open('filelist', 'r'):
		if file_re.match(line):
			print "%s" % file_re.match(line).group(2)

# directory_files --
#	List the directories in filelist.
def directory_files():
	dir_re = re.compile(r'^(\w(\w|/)*)/((\w|.)*)')
	dirs = {}
	for line in open('filelist', 'r'):
		if dir_re.match(line):
			dirs[dir_re.match(line).group(1)] = 1
	for i in dirs.keys():
		print "%s" % i

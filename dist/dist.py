# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

import filecmp, os, re, shutil

# source_files_list --
#	Return a list of the source file names in filelist.
def source_files_list():
	list=[]
	file_re = re.compile(r'^(\w|/)+/((\w|.)+)')
	for line in open('filelist', 'r'):
		if file_re.match(line):
			list.append(file_re.match(line).group(2))
	return sorted(list)

# source_files --
#	Print a list of the source file names in filelist.
def source_files():
	for line in source_files_list():
		print line

# source_paths_list --
#	Return a list of the source file paths in filelist.
def source_paths_list():
	list=[]
	file_re = re.compile(r'^\w')
	for line in open('filelist', 'r'):
		if file_re.match(line):
			list.append(line.rstrip())
	return sorted(list)

# source_paths --
#	Print a list of the source file paths in filelist.
def source_paths():
	for line in source_paths_list():
		print line

# directory_files_list --
#	Return a list of the directories in filelist.
def directory_files_list():
	dirs = {}
	dir_re = re.compile(r'^((\w|/)+/)')
	for line in open('filelist', 'r'):
		if dir_re.match(line):
			dirs[dir_re.match(line).group(1)] = 1
	return sorted(dirs)

# directory_files --
#	Print a list of the directories in filelist.
def directory_files():
	for entry in directory_files_list():
		print entry

# compare_srcfile --
#	Compare two files, and if they differ, update the source file.
def compare_srcfile(tmp, src):
	if not os.path.isfile(src) or \
	    not filecmp.cmp(tmp, src, False):
		print 'Updating ' + src
		shutil.copyfile(tmp, src)

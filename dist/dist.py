# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

import filecmp, os, re, shutil, string, sys
from collections import defaultdict

# api_load --
#       Read and parse the api file into a set of dictionaries:
#
#	    arguments	- a list of the arguments the method takes
#	    config	- a string of the method's keywords
#	    flags	- a string of the method's flags
#	    off		- a list of the method's off transitions
#	    on		- a list of the method's on transitions
#
#	The dictionaries are keyed by the "handle.method" name.
def api_load():
	arguments = defaultdict(list)
	config = defaultdict(str)
	flags = defaultdict(str)
	off = defaultdict(list)
	on = defaultdict(list)
	for line in open('api', 'r').readlines():
		# Skip comments and empty lines.
		if line[:1] == '#' or line[:1] == '\n':
			continue

		# Lines beginning with a tab are additional information for the
		# current method, all other lines are new methods.
		if line[:1] == '\t':
			s = string.split(line.strip(), ':')
			s[1] = s[1].strip()
			if s[0] == 'argument':
				arguments[method].append(s[1])
			elif s[0] == 'config':
				config[method] = s[1]
			elif s[0] == 'flag':
				flag[method] = s[1]
			elif s[0] == 'off':
				off[method].append(s[1])
			elif s[0] == 'on':
				on[method].append(s[1])
			else:
				print >> sys.stderr,\
				    "api: unknown keyword: " + line
				sys.exit(1)
		else:
			method = line.strip()
	return arguments, config, flags, off, on

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

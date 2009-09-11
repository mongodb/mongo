# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Read the source files and output the statistics #defines and allocation code.

import re, string, sys
from dist import compare_srcfile
from dist import source_paths_list

# Read the source files and build a dictionary of handles and stat counters.
def stat_build():
	stats = {}
	stat_re = re.compile\
	    (r'\bWT_STAT_([A-Z]|_)+\(\s*((\w|-|>)+),\s*(([A-Z]|_)+),\s*("([^"]+)")',\
	    re.DOTALL)
	for file in source_paths_list():
		for match in stat_re.finditer(open('../' + file, 'r').read()):
			name = string.split(match.group(2), '->')
			name = name[-2] + "_" + name[-1]
			stats.setdefault\
			    (name,[]).append([match.group(4), match.group(6)])
	return (stats)

# Read the source files and build a dictionary of handles and stat counters.
stats = stat_build()

# Update the #defines in the stat.h file.
tmp_file = '__tmp'
f = open(tmp_file, 'w')
skip = 0
for line in open('../inc_posix/stat.h', 'r'):
	if not skip:
		f.write(line)
	if line.count('Statistics section: END'):
		f.write(line)
		skip = 0
	elif line.count('Statistics section: BEGIN'):
		f.write('\n')
		skip = 1
		for d in sorted(stats.iteritems()):
			def_cnt = 0
			f.write('/*\n')
			f.write(\
			    ' * Statistics entries for ' + d[0].upper() + '\n')
			f.write(' */\n')
			for l in sorted(d[1]):
				n = 'WT_STAT_' + l[0]
				f.write('#define\t' + n +\
				"\t" * max(1, 4 - len(n) / 8) +\
				"%5d" % def_cnt + '\n')
				def_cnt += 1
			n = 'WT_STAT_' + d[0].upper()  + '_TOTAL'
			f.write('#define\t' + n +\
				"\t" * max(1, 4 - len(n) / 8) +\
				"%5d" % def_cnt + '\n')
			f.write('\n')
f.close()
compare_srcfile(tmp_file, '../inc_posix/stat.h')

# Write the stat allocation, clear and print routines to the stat.c file.
f = open(tmp_file, 'w')
f.write('/* DO NOT EDIT: automatically built by dist/stat.py. */\n\n')
f.write('#include "wt_internal.h"\n')
for d in sorted(stats.iteritems()):
	f.write('\n')
	f.write('int\n')
	f.write('__wt_stat_alloc_' + d[0] + '(ENV *env, WT_STATS **statsp)\n')
	f.write('{\n')
	f.write('\tWT_STATS *stats;\n\n')
	f.write('\tWT_RET(__wt_calloc(env,\n')
	f.write('\t    WT_STAT_' + d[0].upper() +\
	    '_TOTAL + 1, sizeof(WT_STATS), &stats));\n\n')
	for l in sorted(d[1]):
		o = '\tstats[WT_STAT_' + l[0] + '].desc = ' + l[1] + ';\n'
		if len(o) + 7  > 80:
			o = o.replace('= ', '=\n\t    ')
		f.write(o)
	f.write('\n')
	f.write('\t*statsp = stats;\n')
	f.write('\treturn (0);\n')
	f.write('}\n')
	f.write('\n')

	f.write('int\n')
	f.write('__wt_stat_clear_' + d[0] + '(WT_STATS *stats)\n')
	f.write('{\n')
	for l in sorted(d[1]):
		f.write('\tstats[WT_STAT_' + l[0] + '].v = 0;\n');
	f.write('\treturn (0);\n')
	f.write('}\n')
f.close()
compare_srcfile(tmp_file, '../support/stat.c')

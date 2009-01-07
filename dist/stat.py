# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Read the source files and output the statistics #defines and allocation code.

import re
from dist import compare_srcfile
from dist import source_paths_list

# Read the source files and build a dictionary of handles and stat counters.
def stat_build():
	stats = {}
	stat_re = re.compile\
	    (r'\bWT_STAT_([A-Z]|_)+\((\w+),\s*((\w|_)+),\s*("([^"]+)")',\
	    re.DOTALL)
	for file in source_paths_list():
		for match in stat_re.finditer(open('../' + file, 'r').read()):
			stats.setdefault(match.group(2),[])\
			    .append([match.group(3), match.group(5)])
	return (stats)


# Read the source files and build a dictionary of handles and stat counters.
stats = stat_build()

# Update the #defines in the stat.h file.
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../inc_posix/stat.h', 'r'):
	if not skip:
		tfile.write(line)
	if line.count('Statistics section: END'):
		tfile.write(line)
		skip = 0
	elif line.count('Statistics section: BEGIN'):
		tfile.write('\n')
		skip = 1
		for d in stats.iteritems():
			def_cnt = 0
			tfile.write('/*\n')
			tfile.write(' * Statistics entries for the ' +\
			    d[0].upper() + ' handle.\n')
			tfile.write(' */\n')
			for l in sorted(d[1]):
				n = 'WT_STAT_' + l[0]
				tfile.write('#define\t' + n +\
				"\t" * max(1, 4 - len(n) / 8) +\
				"%5d" % def_cnt + '\n')
				def_cnt += 1
			n = 'WT_STAT_' + d[0].upper() + '_TOTAL_ENTRIES'
			tfile.write('#define\t' + n +\
				"\t" * max(1, 4 - len(n) / 8) +\
				"%5d" % def_cnt + '\n')
			tfile.write('\n')
tfile.close()
compare_srcfile(tmp_file, '../inc_posix/stat.h')

# Write the allocation source code to the stat.c file.
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/stat.py. */\n\n')
tfile.write('#include "wt_internal.h"\n')
for d in stats.iteritems():
	tfile.write('\n')
	tfile.write('int\n')
	tfile.write('__wt_stat_alloc_' +\
	    d[0] + '(ENV *env, WT_STATS **statsp)\n')
	tfile.write('{\n')
	tfile.write('\tWT_STATS *stats;\n')
	tfile.write('\tint ret;\n\n')
	tfile.write('\tif ((ret = __wt_calloc(env,\n')
	tfile.write('\t    WT_STAT_' + d[0].upper() +\
	    '_TOTAL_ENTRIES + 1, sizeof(WT_STATS), &stats)) != 0)\n')
	tfile.write('\t\treturn (ret);\n\n');
	for l in d[1]:
		o = '\tstats[WT_STAT_' + l[0] + '].desc = ' + l[1] + ';\n'
		if len(o) + 7  > 80:
			o = o.replace('= ', '=\n\t    ')
		tfile.write(o)
	tfile.write('\n')
	tfile.write('\t*statsp = stats;\n')
	tfile.write('\treturn (0);\n')
	tfile.write('}\n')
	tfile.write('\n')

	tfile.write('int\n')
	tfile.write('__wt_stat_clear_' + d[0] + '(WT_STATS *stats)\n')
	tfile.write('{\n')
	for l in d[1]:
		tfile.write('\tstats[WT_STAT_' + l[0] + '].v = 0;\n');
	tfile.write('\treturn (0);\n')
	tfile.write('}\n')
tfile.close()
compare_srcfile(tmp_file, '../support/stat.c')

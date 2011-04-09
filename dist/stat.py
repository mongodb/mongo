# Read the source files and output the statistics #defines and allocation code.

import re, string, sys
from dist import compare_srcfile
from dist import source_paths_list

# Read the source files and build a dictionary of handles and stat counters.
from stat_class import *

# print_def --
#	Print the structures for the stat.h file.
def print_struct(title, name, list):
	f.write('/*\n')
	f.write(' * Statistics entries for ' + title + ' handle.\n')
	f.write(' */\n')
	f.write('struct __wt_' + name + '_stats {\n')
	for l in sorted(list.items()):
		f.write('\tstruct __wt_stats ' + l[0] + ';\n')
	f.write('};\n\n')

# Update the #defines in the stat.h file.
tmp_file = '__tmp'
f = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/stat.h', 'r'):
	if not skip:
		f.write(line)
	if line.count('Statistics section: END'):
		f.write(line)
		skip = 0
	elif line.count('Statistics section: BEGIN'):
		f.write('\n')
		skip = 1
		print_struct('BTREE', 'btree', btree_stats)
		print_struct('BTREE FILE', 'btree_file', btree_file_stats)
		print_struct('CACHE', 'cache', cache_stats)
		print_struct('CONNECTION', 'conn', conn_stats)
		print_struct('FH', 'file', fh_stats)
f.close()
compare_srcfile(tmp_file, '../src/include/stat.h')

# print_func --
#	Print the functions for the stat.c file.
def print_func(name, list):
	f.write('\n')
	f.write('int\n')
	f.write('__wt_stat_alloc_' + name +
	    '_stats(SESSION *session, WT_' +
	    name.upper() + '_STATS **statsp)\n')
	f.write('{\n')
	f.write('\tWT_' + name.upper() + '_STATS *stats;\n\n')
	f.write('\tWT_RET(__wt_calloc_def(session, 1, &stats));\n\n')

	for l in sorted(list.items()):
		o = '\tstats->' + l[0] + '.desc = "' + l[1].str + '";\n'
		if len(o) + 7  > 80:
			o = o.replace('= ', '=\n\t    ')
		f.write(o)
	f.write('\n')
	f.write('\t*statsp = stats;\n')
	f.write('\treturn (0);\n')
	f.write('}\n\n')

	f.write('void\n')
	f.write('__wt_stat_clear_' +
	    name + '_stats(WT_' + name.upper() + '_STATS *stats)\n')
	f.write('{\n')
	for l in sorted(list.items()):
		# Items marked permanent aren't cleared by the stat clear
		# methods.
		if not l[1].config.count('perm'):
			f.write('\tstats->' + l[0] + '.v = 0;\n');
	f.write('}\n\n')

	f.write('void\n')
	f.write('__wt_stat_print_' + name +
	    '_stats(WT_' + name.upper() + '_STATS *stats, FILE *stream)\n')
	f.write('{\n')
	for l in sorted(list.items()):
		f.write('\t__wt_stat_print(&stats->' +
		    l[0] + ', stream);\n');
	f.write('}\n')

# Write the stat allocation, clear and print routines to the stat.c file.
f = open(tmp_file, 'w')
f.write('/* DO NOT EDIT: automatically built by dist/stat.py. */\n\n')
f.write('#include "wt_internal.h"\n')

print_func('btree', btree_stats)
print_func('btree_file', btree_file_stats)
print_func('cache', cache_stats)
print_func('conn', conn_stats)
print_func('file', fh_stats)

f.close()

compare_srcfile(tmp_file, '../src/support/stat.c')

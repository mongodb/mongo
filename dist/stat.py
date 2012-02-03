# Read the source files and output the statistics #defines and allocation code.

import re, string, sys, textwrap
from operator import attrgetter
from dist import compare_srcfile
from dist import source_paths_list

# Read the source files.
from stat_data import btree_stats, connection_stats

# print_struct --
#	Print the structures for the stat.h file.
def print_struct(title, name, list):
	f.write('/*\n')
	f.write(' * Statistics entries for ' + title + ' handle.\n')
	f.write(' */\n')
	f.write('struct __wt_' + name + '_stats {\n')

	# Sort the structure fields by their description, so the eventual
	# disply is sorted by string.
	for l in sorted(list, key=attrgetter('desc')):
		f.write('\tWT_STATS ' + l.name + ';\n')
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
		print_struct('CONNECTION', 'connection', connection_stats)
f.close()
compare_srcfile(tmp_file, '../src/include/stat.h')

# print_define --
#	Print the #defines for the wiredtiger.in file.
def print_define():
	# Sort the structure fields by their description so they match
	# the structure lists.
	f.write('''
/*!
 * @name Statistics for connection handles
 * @anchor statistics_keys
 * @anchor statistics_conn
 * Statistics in WiredTiger are accessed through cursors with \c "statistics:"
 * URIs.  Individual statistics can be queried through the cursor using the
 * following keys.
 * @{
 */
''')
	for v, l in enumerate(sorted(connection_stats, key=attrgetter('desc'))):
		f.write('/*! %s */\n' % '\n * '.join(textwrap.wrap(l.desc, 70)))
		f.write('#define\tWT_STAT_' + l.name + "\t" *
		    max(1, 6 - int((len('WT_STAT_') + len(l.name)) / 8)) +
		    str(v) + '\n')
	f.write('''
/*!
 * @}
 * @name Statistics for file objects
 * @anchor statistics_file
 * @{
 */
''')
	for v, l in enumerate(sorted(btree_stats, key=attrgetter('desc'))):
		f.write('/*! %s */\n' % '\n * '.join(textwrap.wrap(l.desc, 70)))
		f.write('#define\tWT_STAT_' + l.name + "\t" *
		    max(1, 6 - int((len('WT_STAT_') + len(l.name)) / 8)) +
		    str(v) + '\n')
	f.write('/*! @} */\n')

# Update the #defines in the wiredtiger.in file.
tmp_file = '__tmp'
f = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/wiredtiger.in', 'r'):
	if not skip:
		f.write(line)
	if line.count('Statistics section: END'):
		f.write(line)
		skip = 0
	elif line.count('Statistics section: BEGIN'):
		f.write(' */\n')
		skip = 1
		print_define()
		f.write('/*\n')
f.close()
compare_srcfile(tmp_file, '../src/include/wiredtiger.in')

# print_func --
#	Print the functions for the stat.c file.
def print_func(name, list):
	f.write('''
int
__wt_stat_alloc_''' + name + '''_stats(WT_SESSION_IMPL *session, WT_''' +
	    name.upper() + '''_STATS **statsp)
{
\tWT_''' + name.upper() + '''_STATS *stats;

\tWT_RET(__wt_calloc_def(session, 1, &stats));

''')

	for l in sorted(list):
		o = '\tstats->' + l.name + '.desc = "' + l.desc + '";\n'
		if len(o) + 7  > 80:
			o = o.replace('= ', '=\n\t    ')
		f.write(o)
	f.write('''
\t*statsp = stats;
\treturn (0);
}
''')

	f.write('''
void
__wt_stat_clear_''' + name + '''_stats(WT_STATS *stats_arg)
{
\tWT_''' + name.upper() + '''_STATS *stats;

\tstats = (WT_''' + name.upper() + '''_STATS *)stats_arg;
''')
	for l in sorted(list):
		# Items marked permanent aren't cleared by the stat clear
		# methods.
		if not l.config.count('perm'):
			f.write('\tstats->' + l.name + '.v = 0;\n');
	f.write('}\n')

# Write the stat allocation and clear routines to the stat.c file.
f = open(tmp_file, 'w')
f.write('/* DO NOT EDIT: automatically built by dist/stat.py. */\n\n')
f.write('#include "wt_internal.h"\n')

print_func('btree', btree_stats)
print_func('connection', connection_stats)
f.close()
compare_srcfile(tmp_file, '../src/support/stat.c')

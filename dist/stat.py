# Read the source files and output the statistics #defines plus the
# initialize and clear code.

import re, string, sys, textwrap
from dist import compare_srcfile
from dist import source_paths_list

# Read the source files.
from stat_data import dsrc_stats, connection_stats

def print_struct(title, name, stats):
	'''Print the structures for the stat.h file.'''
	f.write('/*\n')
	f.write(' * Statistics entries for ' + title + '.\n')
	f.write(' */\n')
	f.write('struct __wt_' + name + '_stats {\n')

	for l in stats:
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
		print_struct('data sources', 'dsrc', dsrc_stats)
		print_struct('connections', 'connection', connection_stats)
f.close()
compare_srcfile(tmp_file, '../src/include/stat.h')

def print_defines():
	'''Print the #defines for the wiredtiger.in file.'''
	f.write('''
/*!
 * @name Connection statistics
 * @anchor statistics_keys
 * @anchor statistics_conn
 * Statistics are accessed through cursors with \c "statistics:" URIs.
 * Individual statistics can be queried through the cursor using the following
 * keys.  See @ref data_statistics for more information.
 * @{
 */
''')
	for v, l in enumerate(connection_stats):
		f.write('/*! %s */\n' % '\n * '.join(textwrap.wrap(l.desc, 70)))
		f.write('#define\tWT_STAT_CONN_' + l.name.upper() + "\t" *
		    max(1, 6 - int((len('WT_STAT_CONN_' + l.name)) / 8)) +
		    str(v) + '\n')
	f.write('''
/*!
 * @}
 * @name Statistics for data sources
 * @anchor statistics_dsrc
 * @{
 */
''')
	for v, l in enumerate(dsrc_stats):
		f.write('/*! %s */\n' % '\n * '.join(textwrap.wrap(l.desc, 70)))
		f.write('#define\tWT_STAT_DSRC_' + l.name.upper() + "\t" *
		    max(1, 6 - int((len('WT_STAT_DSRC_' + l.name)) / 8)) +
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
		print_defines()
		f.write('/*\n')
f.close()
compare_srcfile(tmp_file, '../src/include/wiredtiger.in')

def print_func(name, list):
	'''Print the functions for the stat.c file.'''
	f.write('''
void
__wt_stat_init_''' + name + '''_stats(WT_''' + name.upper() + '''_STATS *stats)
{
''')
	for l in sorted(list):
		o = '\tstats->' + l.name + '.desc = "' + l.desc + '";\n'
		if len(o) + 7  > 80:
			o = o.replace('= ', '=\n\t    ')
		f.write(o)
	f.write('''}
''')

	f.write('''
void
__wt_stat_clear_''' + name + '''_stats(void *stats_arg)
{
\tWT_''' + name.upper() + '''_STATS *stats;

\tstats = (WT_''' + name.upper() + '''_STATS *)stats_arg;
''')
	for l in sorted(list):
		# Items marked permanent aren't cleared by the stat clear
		# methods.
		if not l.flags.get('perm', 0):
			f.write('\tstats->' + l.name + '.v = 0;\n');
	f.write('}\n')

# Write the stat initialization and clear routines to the stat.c file.
f = open(tmp_file, 'w')
f.write('/* DO NOT EDIT: automatically built by dist/stat.py. */\n\n')
f.write('#include "wt_internal.h"\n')

print_func('dsrc', dsrc_stats)
print_func('connection', connection_stats)
f.close()
compare_srcfile(tmp_file, '../src/support/stat.c')

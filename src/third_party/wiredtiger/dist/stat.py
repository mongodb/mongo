# Read the source files and output the statistics #defines plus the
# initialize and refresh code.

import re, string, sys, textwrap
from dist import compare_srcfile

# Read the source files.
from stat_data import groups, dsrc_stats, connection_stats

def print_struct(title, name, base, stats):
    '''Print the structures for the stat.h file.'''
    f.write('/*\n')
    f.write(' * Statistics entries for ' + title + '.\n')
    f.write(' */\n')
    f.write(
        '#define\tWT_' + name.upper() + '_STATS_BASE\t' + str(base) + '\n')
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
        print_struct(
            'connections', 'connection', 1000, connection_stats)
        print_struct('data sources', 'dsrc', 2000, dsrc_stats)
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
    for v, l in enumerate(connection_stats, 1000):
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
    for v, l in enumerate(dsrc_stats, 2000):
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
\t/* Clear, so can also be called for reinitialization. */
\tmemset(stats, 0, sizeof(*stats));

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
__wt_stat_refresh_''' + name + '''_stats(void *stats_arg)
{
\tWT_''' + name.upper() + '''_STATS *stats;

\tstats = (WT_''' + name.upper() + '''_STATS *)stats_arg;
''')
    for l in sorted(list):
        # no_clear: don't clear the value.
        if not 'no_clear' in l.flags:
            f.write('\tstats->' + l.name + '.v = 0;\n');
    f.write('}\n')

    # Aggregation is only interesting for data-source statistics.
    if name == 'connection':
        return;

    f.write('''
void
__wt_stat_aggregate_''' + name +
'''_stats(const void *child, const void *parent)
{
\tWT_''' + name.upper() + '''_STATS *c, *p;

\tc = (WT_''' + name.upper() + '''_STATS *)child;
\tp = (WT_''' + name.upper() + '''_STATS *)parent;
''')
    for l in sorted(list):
        if 'no_aggregate' in l.flags:
            continue;
        elif 'max_aggregate' in l.flags:
            o = 'if (c->' + l.name + '.v > p->' + l.name +\
            '.v)\n\t    p->' + l.name + '.v = c->' + l.name + '.v;'
        else:
            o = 'p->' + l.name + '.v += c->' + l.name + '.v;'
        f.write('\t' + o + '\n')
    f.write('}\n')

# Write the stat initialization and refresh routines to the stat.c file.
f = open(tmp_file, 'w')
f.write('/* DO NOT EDIT: automatically built by dist/stat.py. */\n\n')
f.write('#include "wt_internal.h"\n')

print_func('dsrc', dsrc_stats)
print_func('connection', connection_stats)
f.close()
compare_srcfile(tmp_file, '../src/support/stat.c')


# Update the statlog file with the entries we can scale per second.
scale_info = 'no_scale_per_second_list = [\n'
clear_info = 'no_clear_list = [\n'
prefix_list = []
for l in sorted(connection_stats):
    prefix_list.append(l.prefix)
    if 'no_scale' in l.flags:
        scale_info += '    \'' + l.desc + '\',\n'
    if 'no_clear' in l.flags:
        clear_info += '    \'' + l.desc + '\',\n'
for l in sorted(dsrc_stats):
    prefix_list.append(l.prefix)
    if 'no_scale' in l.flags:
        scale_info += '    \'' + l.desc + '\',\n'
    if 'no_clear' in l.flags:
        clear_info += '    \'' + l.desc + '\',\n'
scale_info += ']\n'
clear_info += ']\n'
prefix_info = 'prefix_list = [\n'
# Remove the duplicates and print out the list
for l in list(set(prefix_list)):
    prefix_info += '    \'' + l + '\',\n'
prefix_info += ']\n'
group_info = 'groups = ' + str(groups)

tmp_file = '__tmp'
f = open(tmp_file, 'w')
f.write('# DO NOT EDIT: automatically built by dist/stat.py. */\n\n')
f.write(scale_info)
f.write(clear_info)
f.write(prefix_info)
f.write(group_info)
f.close()
compare_srcfile(tmp_file, '../tools/wtstats/stat_data.py')

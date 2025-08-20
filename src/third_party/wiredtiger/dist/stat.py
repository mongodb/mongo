#!/usr/bin/env python3

# Read the source files and output the statistics #defines plus the
# initialize and refresh code.

from collections import defaultdict
import re
import os, sys, textwrap
from dist import compare_srcfile, format_srcfile
from operator import attrgetter
from common_functions import filter_if_fast

if not [f for f in filter_if_fast([
            "../dist/dist.py",
            "../dist/stat_data.py",
            "../src/include/stat.h",
            "../src/include/wiredtiger.h.in",
            "../src/support/stat.c",
        ], prefix="../")]:
    sys.exit(0)

# Read the source files.
from stat_data import dsrc_stats, conn_stats, conn_dsrc_stats, session_stats

##########################################
# Check for duplicate stat descriptions:
# Duplicate stat descpriptions within a category are not allowed.
# The list must be sorted by description.
##########################################
def check_unique_description(sorted_list):
    temp = ""
    for i in sorted_list:
        if temp == i.desc:
            print("ERROR: repeated stat description in - '%s'" % (i.desc))
        temp = i.desc

##########################################
# Remove trailing digits for a string.
##########################################
def remove_suffix_digits(str):
    return re.sub(r'\d+$', '', str)
    
##########################################
# For each stat subclass check the names are sorted in alphabetical order.
##########################################
def check_name_sorted(stat_list):
    stat_dict = defaultdict(list)
    for stat in stat_list:
        stat_dict[type(stat)].append(stat)
    for stat_type, stats in stat_dict.items():
        # In alphabetical order, stat_name_100 comes before stat_name_50. 
        # For this reason, remove any numerical suffix before sorting the stats. 
        # Print an error if the stats are not sorted correctly.
        sorted_stats = sorted(stats, key=lambda stat: remove_suffix_digits(stat.name))
        for sorted_stat, stat in zip(sorted_stats, stats):
            if sorted_stat.name != stat.name:
                print(f"ERROR: {stat_type.__name__} not sorted alphabetically by name, expected " \
                      f"'{sorted_stat.name}' but found '{stat.name}'")
                return

all_stat_list = [conn_dsrc_stats, conn_stats, dsrc_stats, session_stats]
for stat_list in all_stat_list:
    check_name_sorted(stat_list)

conn_dsrc_stats.sort(key=attrgetter('desc'))
conn_stats.sort(key=attrgetter('desc'))
dsrc_stats.sort(key=attrgetter('desc'))
session_stats.sort(key=attrgetter('desc'))

check_unique_description(conn_dsrc_stats)
check_unique_description(conn_stats)
check_unique_description(dsrc_stats)
check_unique_description(session_stats)

# Statistic categories need to be sorted in order to generate a valid statistics JSON file.
sorted_conn_stats = conn_stats
sorted_conn_stats.extend(conn_dsrc_stats)
sorted_conn_stats = sorted(sorted_conn_stats, key=attrgetter('desc'))
sorted_dsrc_statistics = dsrc_stats
sorted_dsrc_statistics.extend(conn_dsrc_stats)
sorted_dsrc_statistics = sorted(sorted_dsrc_statistics, key=attrgetter('desc'))

def print_struct(title, name, base, stats):
    '''Print the structures for the stat.h file.'''
    f.write('/*\n')
    f.write(' * Statistics entries for ' + title + '.\n')
    f.write(' */\n')
    f.write('#define\tWT_' + name.upper() + '_STATS_BASE\t' + str(base) + '\n')
    f.write('struct __wt_' + name + '_stats {\n')

    for l in stats:
        f.write('\tint64_t ' + l.name + ';\n')
    f.write('};\n\n')

# Update the #defines in the stat.h file.
tmp_file = '__tmp_stat' + str(os.getpid())
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
        print_struct('connections', 'connection', 1000, sorted_conn_stats)
        print_struct('data sources', 'dsrc', 2000, sorted_dsrc_statistics)
        print_struct('session', 'session', 4000, session_stats)
f.close()
format_srcfile(tmp_file)
compare_srcfile(tmp_file, '../src/include/stat.h')

def print_defines_one(capname, base, stats):
    for v, l in enumerate(stats, base):
        desc = l.desc
        if 'cache_walk' in l.flags:
            desc += \
                ', only reported if cache_walk or all statistics are enabled'
        if 'tree_walk' in l.flags:
            desc += ', only reported if tree_walk or all statistics are enabled'
        if len(textwrap.wrap(desc, 70)) > 1:
            f.write('/*!\n')
            f.write(' * %s\n' % '\n * '.join(textwrap.wrap(desc, 70)))
            f.write(' */\n')
        else:
            f.write('/*! %s */\n' % desc)
        #f.write('/*! %s */\n' % '\n * '.join(textwrap.wrap(desc, 70)))
        f.write('#define\tWT_STAT_' + capname + '_' + l.name.upper() + "\t" *
            max(1, 6 - int((len('WT_STAT_' + capname + '_' + l.name)) / 8)) +
            str(v) + '\n')

def print_defines():
    '''Print the #defines for the wiredtiger.h.in file.'''
    f.write('''
/*!
 * @name Connection statistics
 * @anchor statistics_keys
 * @anchor statistics_conn
 * Statistics are accessed through cursors with \\c "statistics:" URIs.
 * Individual statistics can be queried through the cursor using the following
 * keys.  See @ref data_statistics for more information.
 * @{
 */
''')
    print_defines_one('CONN', 1000, sorted_conn_stats)
    f.write('''
/*!
 * @}
 * @name Statistics for data sources
 * @anchor statistics_dsrc
 * @{
 */
''')
    print_defines_one('DSRC', 2000, sorted_dsrc_statistics)
    f.write('''
/*!
 * @}
 * @name Statistics for session
 * @anchor statistics_session
 * @{
 */
''')
    print_defines_one('SESSION', 4000, session_stats)
    f.write('/*! @} */\n')

# Update the #defines in the wiredtiger.h.in file.
tmp_file = '__tmp_stat' + str(os.getpid())
f = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/wiredtiger.h.in', 'r'):
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
compare_srcfile(tmp_file, '../src/include/wiredtiger.h.in')

def print_func(name, handle, statlist, capname=None):
    '''Print the structures/functions for the stat.c file.'''
    f.write('\n')
    f.write('static const char * const __stats_' + name + '_desc[] = {\n')
    for l in statlist:
        f.write('\t"' + l.desc + '",\n')
    f.write('};\n')

    f.write('''
int
__wt_stat_''' + name + '''_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
{
\tWT_UNUSED(cst);
\t*p = __stats_''' + name + '''_desc[slot];
\treturn (0);
}
''')

    f.write('''
void
__wt_stat_''' + name + '_init_single(WT_' + name.upper() + '''_STATS *stats)
{
\tmemset(stats, 0, sizeof(*stats));
}
''')

    if handle != None:
        f.write('''
int
__wt_stat_''' + name + '''_init(
    WT_SESSION_IMPL *session, ''' + handle + ''' *handle)
{
\tint i;

\tWT_RET(__wt_calloc(session, (size_t)WT_STAT_''' + capname + '''_COUNTER_SLOTS,
\t    sizeof(*handle->stat_array), &handle->stat_array));

\tfor (i = 0; i < WT_STAT_''' + capname + '''_COUNTER_SLOTS; ++i) {
\t\thandle->stats[i] = &handle->stat_array[i];
\t\t__wt_stat_''' + name + '''_init_single(handle->stats[i]);
\t}
\treturn (0);
}

void
__wt_stat_''' + name + '''_discard(
    WT_SESSION_IMPL *session, ''' + handle + ''' *handle)
{
\t__wt_free(session, handle->stat_array);
}
''')

    f.write('''
void
__wt_stat_''' + name + '_clear_single(WT_' + name.upper() + '''_STATS *stats)
{
''')
    for l in statlist:
        # no_clear: don't clear the value.
        if 'no_clear' in l.flags:
            f.write('\t\t/* not clearing ' + l.name + ' */\n')
        else:
            f.write('\tstats->' + l.name + ' = 0;\n')
    f.write('}\n')

    if name != 'session':
        f.write('''
void
__wt_stat_''' + name + '_clear_all(WT_' + name.upper() + '''_STATS **stats)
{
\tu_int i;

\tfor (i = 0; i < WT_STAT_''' + capname + '''_COUNTER_SLOTS; ++i)
\t\t__wt_stat_''' + name + '''_clear_single(stats[i]);
}
''')

    # Single structure aggregation is currently only used by data sources.
    if name == 'dsrc':
        f.write('''
void
__wt_stat_''' + name + '''_aggregate_single(
    WT_''' + name.upper() + '_STATS *from, WT_' + name.upper() + '''_STATS *to)
{
''')
        for l in statlist:
            if 'max_aggregate' in l.flags:
                o = '\tif (from->' + l.name + ' > to->' + l.name + ')\n' +\
                    '\t\tto->' + l.name + ' = from->' + l.name + ';\n'
            else:
                o = '\tto->' + l.name + ' += from->' + l.name + ';\n'
                if len(o) > 72:         # Account for the leading tab.
                    o = o.replace(' += ', ' +=\n\t    ')
            f.write(o)
        f.write('}\n')

    if name != 'session':
        f.write('''
void
__wt_stat_''' + name + '''_aggregate(
    WT_''' + name.upper() + '_STATS **from, WT_' + name.upper() + '''_STATS *to)
{
''')
        # Connection level aggregation does not currently have any computation
        # of a maximum value; I'm leaving in support for it, but don't declare
        # a temporary variable until it's needed.
        for l in statlist:
            if 'max_aggregate' in l.flags:
                f.write('\tint64_t v;\n\n')
                break;
        for l in statlist:
            if 'max_aggregate' in l.flags:
                o = '\tif ((v = WT_STAT_' + capname + '_READ(from, ' + l.name + ')) > ' +\
                    'to->' + l.name + ')\n'
                if len(o) > 72:             # Account for the leading tab.
                    o = o.replace(' > ', ' >\n\t    ')
                o +='\t\tto->' + l.name + ' = v;\n'
            else:
                o = '\tto->' + l.name + ' += WT_STAT_' + capname + '_READ(from, ' + l.name +\
                    ');\n'
                if len(o) > 72:             # Account for the leading tab.
                    o = o.replace(' += ', ' +=\n\t    ')
            f.write(o)
        f.write('}\n')

# Write the stat initialization and refresh routines to the stat.c file.
f = open(tmp_file, 'w')
f.write('/* DO NOT EDIT: automatically built by dist/stat.py. */\n\n')
f.write('#include "wt_internal.h"\n')

print_func('dsrc', 'WT_DATA_HANDLE', sorted_dsrc_statistics, 'DSRC')
print_func('connection', 'WT_CONNECTION_IMPL', sorted_conn_stats, 'CONN')
print_func('session', None, session_stats)
f.close()
format_srcfile(tmp_file)
compare_srcfile(tmp_file, '../src/support/stat.c')

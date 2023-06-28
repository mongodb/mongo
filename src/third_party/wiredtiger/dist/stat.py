# Read the source files and output the statistics #defines plus the
# initialize and refresh code.

import re, string, sys, textwrap
from dist import compare_srcfile, format_srcfile
from operator import attrgetter

# Read the source files.
from stat_data import groups, dsrc_stats, conn_stats, conn_dsrc_stats, join_stats, \
    session_stats

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

check_unique_description(conn_stats)
check_unique_description(dsrc_stats)
check_unique_description(session_stats)
check_unique_description(join_stats)
check_unique_description(conn_dsrc_stats)

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
        print_struct('connections', 'connection', 1000, sorted_conn_stats)
        print_struct('data sources', 'dsrc', 2000, sorted_dsrc_statistics)
        print_struct('join cursors', 'join', 3000, join_stats)
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
 * @name Statistics for join cursors
 * @anchor statistics_join
 * @{
 */
''')
    print_defines_one('JOIN', 3000, join_stats)
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

def print_func(name, handle, statlist):
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

\tWT_RET(__wt_calloc(session, (size_t)WT_COUNTER_SLOTS,
\t    sizeof(*handle->stat_array), &handle->stat_array));

\tfor (i = 0; i < WT_COUNTER_SLOTS; ++i) {
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

\tfor (i = 0; i < WT_COUNTER_SLOTS; ++i)
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
                o = '\tif ((v = WT_STAT_READ(from, ' + l.name + ')) > ' +\
                    'to->' + l.name + ')\n'
                if len(o) > 72:             # Account for the leading tab.
                    o = o.replace(' > ', ' >\n\t    ')
                o +='\t\tto->' + l.name + ' = v;\n'
            else:
                o = '\tto->' + l.name + ' += WT_STAT_READ(from, ' + l.name +\
                    ');\n'
                if len(o) > 72:             # Account for the leading tab.
                    o = o.replace(' += ', ' +=\n\t    ')
            f.write(o)
        f.write('}\n')

# Write the stat initialization and refresh routines to the stat.c file.
f = open(tmp_file, 'w')
f.write('/* DO NOT EDIT: automatically built by dist/stat.py. */\n\n')
f.write('#include "wt_internal.h"\n')

print_func('dsrc', 'WT_DATA_HANDLE', sorted_dsrc_statistics)
print_func('connection', 'WT_CONNECTION_IMPL', sorted_conn_stats)
print_func('join', None, join_stats)
print_func('session', None, session_stats)
f.close()
format_srcfile(tmp_file)
compare_srcfile(tmp_file, '../src/support/stat.c')

#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# [TEST_TAGS]
# schema_api
# indexes
# [END_TAGS]

import os
import suite_random
from helper_tiered import TieredConfigMixin, tiered_storage_sources
import wtscenario, wttest

try:
    # Windows does not getrlimit/setrlimit so we must catch the resource
    # module load
    import resource
except:
    None

# test_schema03.py
#    Bigger, more 'randomly generated' schemas and data.
#    This test is complex.  If it fails, rerun with modified values for
# SHOW_PYTHON* variables.
def extract_random_from_list(rand, list):
    pos = rand.rand_range(0, len(list))
    result = list[pos]
    list = list[:pos] + list[pos+1:]
    return (result, list)

class tabconfig:
    """
    Configuration for a table used in the test
    """
    def __init__(self):
        self.tableidx = -1
        self.tablename = ''
        self.cglist = []   # list of related cgconfig
        self.idxlist = []  # list of related idxconfig
        self.nkeys = 0     # how many key columns
        self.nvalues = 0   # how many value columns
        self.nentries = 0
        self.keyformats = ''
        self.valueformats = ''

    # we don't want to insert the keys in order,
    # so generate them with backwards digits e.g.
    # 235 => 532.  However, 100 backwards is 001,
    # so we append a positive integer to the end
    # before reversing.
    def gen_keys(self, i):
        addmod = i * 10 + (i % 7) + 1
        rev = int((str(addmod))[::-1])
        keys = []
        # ASSUME: each format is 1 char
        for format in self.keyformats:
            if format == 'S':
                keys.append(str(rev))
            elif format == 'i':
                keys.append(rev)
            elif format == 'r':
                keys.append(self.recno(i+1))
        return keys

    def gen_values(self, i):
        vals = []
        # ASSUME: each format is 1 char
        for format in self.valueformats:
            if format == 'S':
                vals.append(str(i))
            elif format == 'i':
                vals.append(i)
        return vals

    def columns_for_groups(self, collist):
        totalgroups = len(self.cglist)
        ncolumns = len(collist)
        rand = suite_random.suite_random(ncolumns, totalgroups)

        # Each columngroup must have at least one column, so
        # the only choice about distribution is with the
        # excess columns.
        excess = ncolumns - totalgroups
        if excess < 0:
            raise ValueError('columns_for_groups expects a column list (len=' + str(ncolumns) + ') larger than column group list (len=' + str(totalgroups) + ')')

        # Initially, all groups get column from the collist
        for cg in self.cglist:
            (colno, collist) = extract_random_from_list(rand, collist)
            cg.columns.append(colno)

        # Then divy up remainder in the collist
        for i in range(0, excess):
            pos = rand.rand_range(0, totalgroups)
            cg = self.cglist[pos]
            (colno, collist) = extract_random_from_list(rand, collist)
            cg.columns.append(colno)

        # collist should be emptied
        if len(collist) != 0:
            raise AssertionError('column list did not get emptied')

    def columns_for_indices(self, collist):
        totalindices = len(self.idxlist)
        ncolumns = len(collist)
        startcol = 0

        # KNOWN LIMITATION: Indices should not include primary keys
        # Remove this statement when the limitation is fixed.
        #startcol = self.nkeys
        # END KNOWN LIMITATION.

        rand = suite_random.suite_random(ncolumns, totalindices)

        # Initially, all indices get one column from the collist.
        # Overlaps are allowed.  Then probabalistically, add some
        # more columns.
        for idx in self.idxlist:
            prob = 1.0
            for i in range(0, ncolumns - startcol):
                if rand.rand_float() > prob:
                    break
                colno = collist[rand.rand_range(startcol, ncolumns)]
                if not any(x == colno for x in idx.columns):
                    idx.columns.append(colno)
                    if colno < self.nkeys:
                        # ASSUME: each format is 1 char
                        idx.formats += self.keyformats[colno]
                    else:
                        # ASSUME: each format is 1 char
                        idx.formats += self.valueformats[colno - self.nkeys]
                prob *= 0.5

class cgconfig:
    """
    Configuration for a column group used in the test.
    Each tabconfig contains a list of these.
    """
    def __init__(self):
        self.cgname = ''
        self.columns = []
        self.createset = 0    # 0 or 1 depending on which set to create them.

class idxconfig:
    """
    Configuration for an index used in the test.
    Each tabconfig contains a list of these.
    """
    def __init__(self):
        self.idxname = ''
        self.columns = []
        self.createset = 0    # 0 or 1 depending on which set to create them.
        self.formats = ''     # piece
        self.tab = None       # references the tabconfig

    def gen_keys(self, i):
        keys = []
        colpos = 0
        addmod = i * 10 + (i % 7) + 1
        rev = int((str(addmod))[::-1])
        for format in self.formats:
            if self.columns[colpos] >= self.tab.nkeys:
                # The column is a value in the primary table
                key = i
            else:
                # The column is a key in the primary table
                key = rev
            if format == 'S':
                key = str(key)
            keys.append(key)
            colpos += 1
        return keys

class test_schema03(TieredConfigMixin, wttest.WiredTigerTestCase):
    """
    Test schemas - a 'predictably random' assortment of columns,
    column groups and indices are created within tables, and are
    created in various orders as much as the API allows.  On some runs
    the connection will be closed and reopened at a particular point
    to test that the schemas (and data) are saved and read correctly.

    The test is run multiple times, using scenarios.
    The test always follows these steps:
    - table:      create tables
    - colgroup0:  create (some) colgroups
    - index0:     create (some) indices
    - colgroup1:  create (more) colgroups
    - index1:     create (more) indices
    - populate0:  populate 1st time
    - index2:     create (more) indices
    - populate1:  populate 2nd time (more key/values)
    - check:      check key/values

    The variations represented by scenarios are:
    - how many tables to create
    - how many colgroups to create at each step (may be 0)
    - how many indices to create at each step (may be 0)
    - between each step, whether to close/reopen the connection
    """

    # Boost cache size and number of sessions for this test
    conn_config_string = 'cache_size=100m,session_max=1000,'

    ################################################################
    # These three variables can be altered to help generate
    # and pare down failing test cases.

    # Set to true to get python test program fragment on stdout,
    # used by show_python() below.
    SHOW_PYTHON = False

    # When SHOW_PYTHON is set, we print an enormous amount of output.
    # To only print for a given scenario, set this
    SHOW_PYTHON_ONLY_SCEN = None  # could be e.g. [2] or [0,1]

    # To print verbosely for only a given table, set this
    SHOW_PYTHON_ONLY_TABLE = None # could be e.g. [2] or [0,1]

    ################################################################

    # Set whenever we are working with a table
    current_table = None

    nentries = 50

    # We need to have a large number of open files available
    # to run this test.  We probably don't need quite this many,
    # but boost it up to this limit anyway.
    OPEN_FILE_LIMIT = 1000

    restart_scenarios = [('table', dict(s_restart=['table'],P=0.3)),
                         ('colgroup0', dict(s_restart=['colgroup0'],P=0.3)),
                         ('index0', dict(s_restart=['index0'],P=0.3)),
                         ('colgroup1', dict(s_restart=['colgroup1'],P=0.3)),
                         ('index1', dict(s_restart=['index1'],P=0.3)),
                         ('populate0', dict(s_restart=['populate0'],P=0.3)),
                         ('index2', dict(s_restart=['index2'],P=0.3)),
                         ('populate1', dict(s_restart=['populate1'],P=0.3)),
                         ('ipop', dict(s_restart=['index0','populate0'],P=0.3)),
                         ('all', dict(s_restart=['table','colgroup0','index0','colgroup1','index1','populate0','index2','populate1'],P=1.0)),
    ]

    ntable_scenarios = wtscenario.quick_scenarios('s_ntable',
        [1,2,5,8], [1.0,0.4,0.5,0.5])
    ncolgroup_scenarios = wtscenario.quick_scenarios('s_colgroup',
        [[1,0],[0,1],[2,4],[8,5]], [1.0,0.2,0.3,1.0])
    nindex_scenarios = wtscenario.quick_scenarios('s_index',
        [[1,1,1],[3,2,1],[5,1,3]], [1.0,0.5,1.0])
    idx_args_scenarios = wtscenario.quick_scenarios('s_index_args',
        ['', ',type=file', ',type=lsm'], [0.5, 0.3, 0.2])
    table_args_scenarios = wtscenario.quick_scenarios('s_extra_table_args',
        ['', ',type=file', ',type=lsm'], [0.5, 0.3, 0.2])

    scenarios = wtscenario.make_scenarios(
        tiered_storage_sources, restart_scenarios, ntable_scenarios,
        ncolgroup_scenarios, nindex_scenarios, idx_args_scenarios,
        table_args_scenarios, prune=30)

    # Note: the set can be reduced here for debugging, e.g.
    # scenarios = scenarios[40:44]
    #   or
    # scenarios = [ scenarios[0], scenarios[30], scenarios[40] ]

    #wttest.WiredTigerTestCase.printVerbose(2, 'test_schema03: running ' + \
    #                      str(len(scenarios)) + ' of ' + \
    #                      str(len(all_scenarios)) + ' possible scenarios')

    # This test requires a large number of open files.
    # Increase our resource limits before we start
    def setUp(self):
        if os.name == "nt":
            self.skipTest('Unix specific test skipped on Windows')

        self.origFileLimit = resource.getrlimit(resource.RLIMIT_NOFILE)
        newlimit = (self.OPEN_FILE_LIMIT, self.origFileLimit[1])
        if newlimit[0] > newlimit[1]:
            self.skipTest('Require %d open files, only %d available' % newlimit)
        resource.setrlimit(resource.RLIMIT_NOFILE, newlimit)
        super(test_schema03, self).setUp()

    # Set up connection config.
    def conn_config(self):
        return self.conn_config_string + self.tiered_conn_config()

    def tearDown(self):
        super(test_schema03, self).tearDown()
        resource.setrlimit(resource.RLIMIT_NOFILE, self.origFileLimit)

    def gen_formats(self, rand, n, iskey):
        result = ''
        for i in range(0, n):
            if rand.rand_range(0, 2) == 0:
                result += 'S'
            else:
                result += 'i'
        return result

    def show_python(self, s):
        if self.SHOW_PYTHON:
            if self.SHOW_PYTHON_ONLY_TABLE == None or self.current_table in self.SHOW_PYTHON_ONLY_TABLE:
                if self.SHOW_PYTHON_ONLY_SCEN == None or self.scenario_number in self.SHOW_PYTHON_ONLY_SCEN:
                    print('        ' + s)

    def join_names(self, sep, prefix, list):
        return sep.join([prefix + str(val) for val in list])

    def create(self, what, tablename, whatname, columnlist, extra_args=''):
        createarg = what + ":" + tablename + ":" + whatname
        colarg = self.join_names(',', 'c', columnlist)
        self.show_python("self.session.create('" + createarg + "', 'columns=(" + colarg + ")" + extra_args + "')")
        result = self.session.create(createarg,
                "columns=(" + colarg + ")" + extra_args)
        self.assertEqual(result, 0)

    def finished_step(self, name):
        if self.s_restart == name:
            print("  # Reopening connection at step: " + name)
            self.reopen_conn()

    def test_schema(self):
        if self.is_tiered_scenario() and (self.s_index_args == ',type=lsm' or self.s_index_args == ',type=file' or
            self.s_extra_table_args == ',type=lsm' or self.s_extra_table_args == ',type=file'):
            self.skipTest('Tiered storage does not support LSM or file URIs.')

        rand = suite_random.suite_random()
        if self.SHOW_PYTHON:
            print('  ################################################')
            print('  # Running scenario ' + str(self.scenario_number))

        ntables = self.s_ntable

        # Report known limitations in the test,
        # we'll work around these later, in a loop where we don't want to print.
        self.KNOWN_LIMITATION('Column groups created after indices confuses things')

        # Column groups are created in two different times.
        # We call these two batches 'createsets'.
        # So we don't have the exactly the same number of column groups
        # for each table, for tests that indicate >1 colgroup, we
        # increase the number of column groups for each table
        tabconfigs = []
        for i in range(0, ntables):
            self.current_table = i
            tc = tabconfig()
            tc.tablename = 't' + str(i)
            tc.tableidx = i
            tabconfigs.append(tc)

            for createset in range(0, 2):
                ncg = self.s_colgroup[createset]
                if ncg > 1:
                    ncg += i
                for k in range(0, ncg):
                    thiscg = cgconfig()
                    thiscg.createset = createset

                    # KNOWN LIMITATION: Column groups created after
                    # indices confuses things.  So for now, put all
                    # column group creation in the first set.
                    # Remove this statement when the limitation is fixed.
                    thiscg.createset = 0
                    # END KNOWN LIMITATION

                    thiscg.cgname = 'g' + str(len(tc.cglist))
                    tc.cglist.append(thiscg)

            # The same idea for indices, except that we create them in
            # three sets
            for createset in range(0, 3):
                nindex = self.s_index[createset]
                if nindex > 1:
                    nindex += i
                for k in range(0, nindex):
                    thisidx = idxconfig()
                    thisidx.createset = createset
                    thisidx.idxname = 'i' + str(len(tc.idxlist))
                    thisidx.tab = tc
                    tc.idxlist.append(thisidx)

            # We'll base the number of key/value columns
            # loosely on the number of column groups and indices.

            colgroups = len(tc.cglist)
            indices = len(tc.idxlist)
            nall = colgroups * 2 + indices
            k = rand.rand_range(1, nall)
            v = rand.rand_range(0, nall)
            # we need at least one value per column group
            if v < colgroups:
                v = colgroups
            tc.nkeys = k
            tc.nvalues = v
            tc.keyformats = self.gen_formats(rand, tc.nkeys, True)
            tc.valueformats = self.gen_formats(rand, tc.nvalues, False)

            # Simple naming (we'll test odd naming elsewhere):
            #  tables named 't0' --> 't<N>'
            #  within each table:
            #     columns named 'c0' --> 'c<N>'
            #     colgroups named 'g0' --> 'g<N>'
            #     indices named 'i0' --> 'i<N>'

            config = ""
            config += "key_format=" + tc.keyformats
            config += ",value_format=" + tc.valueformats
            config += ",columns=("
            for j in range(0, tc.nkeys + tc.nvalues):
                if j != 0:
                    config += ","
                config += "c" + str(j)
            config += "),colgroups=("
            for j in range(0, len(tc.cglist)):
                if j != 0:
                    config += ","
                config += "g" + str(j)
            config += ")"
            config += self.s_extra_table_args
            # indices are not declared here
            self.show_python("self.session.create('table:" + tc.tablename + "', '" + config + "')")
            self.session.create("table:" + tc.tablename, config)

            tc.columns_for_groups(list(range(tc.nkeys, tc.nkeys + tc.nvalues)))
            tc.columns_for_indices(list(range(0, tc.nkeys + tc.nvalues)))

        self.finished_step('table')

        for createset in (0, 1):
            # Create column groups in this set
            # e.g. self.session.create("colgroup:t0:g1", "columns=(c3,c4)")
            for tc in tabconfigs:
                self.current_table = tc.tableidx
                for cg in tc.cglist:
                    if cg.createset == createset:
                        self.create('colgroup', tc.tablename, cg.cgname, cg.columns)

            self.finished_step('colgroup' + str(createset))

            # Create indices in this set
            # e.g. self.session.create("index:t0:i1", "columns=(c3,c4)")
            for tc in tabconfigs:
                self.current_table = tc.tableidx
                for idx in tc.idxlist:
                    if idx.createset == createset:
                        self.create('index', tc.tablename, idx.idxname, idx.columns, self.s_index_args)

            self.finished_step('index' + str(createset))

        # populate first batch
        for tc in tabconfigs:
            self.current_table = tc.tableidx
            max = rand.rand_range(0, self.nentries)
            self.populate(tc, list(range(0, max)))

        self.finished_step('populate0')

        # Create indices in third set
        for tc in tabconfigs:
            for idx in tc.idxlist:
                if idx.createset == 2:
                    self.create('index', tc.tablename, idx.idxname, idx.columns)

        self.finished_step('index2')

        # populate second batch
        for tc in tabconfigs:
            self.current_table = tc.tableidx
            self.populate(tc, list(range(tc.nentries, self.nentries)))

        self.finished_step('populate1')

        for tc in tabconfigs:
            self.current_table = tc.tableidx
            self.check_entries(tc)

    def populate(self, tc, insertrange):
        self.show_python("cursor = self.session.open_cursor('table:" + tc.tablename + "', None, None)")
        cursor = self.session.open_cursor('table:' + tc.tablename, None, None)
        for i in insertrange:
            key = tc.gen_keys(i)
            val = tc.gen_values(i)
            self.show_python("cursor.set_key(*" + str(key) + ")")
            cursor.set_key(*key)
            self.show_python("cursor.set_value(*" + str(val) + ")")
            cursor.set_value(*val)
            self.show_python("cursor.insert()")
            cursor.insert()
            tc.nentries += 1
        self.show_python("cursor.close()")
        cursor.close()

    def check_one(self, name, cursor, key, val):
        keystr = str(key)
        valstr = str(val)
        self.show_python('# search[' + name + '](' + keystr + ')')
        self.show_python("cursor.set_key(*" + keystr + ")")
        cursor.set_key(*key)
        self.show_python("ok = cursor.search()")
        ok = cursor.search()
        self.show_python("self.assertEqual(ok, 0)")
        self.assertEqual(ok, 0)
        self.show_python("self.assertEqual(" + keystr + ", cursor.get_keys())")
        self.assertEqual(key, cursor.get_keys())
        self.show_python("self.assertEqual(" + valstr + ", cursor.get_values())")
        self.assertEqual(val, cursor.get_values())

    def check_entries(self, tc):
        """
        Verify entries in the primary and index table
        related to the tabconfig.
        """
        self.show_python('# check_entries: ' + tc.tablename)
        self.show_python("cursor = self.session.open_cursor('table:" + tc.tablename + "', None, None)")
        cursor = self.session.open_cursor('table:' + tc.tablename, None, None)
        count = 0
        for x in cursor:
            count += 1
        self.assertEqual(count, tc.nentries)
        for i in range(0, tc.nentries):
            key = tc.gen_keys(i)
            val = tc.gen_values(i)
            self.check_one(tc.tablename, cursor, key, val)
        cursor.close()
        self.show_python("cursor.close()")

        # for each index, check each entry
        for idx in tc.idxlist:
            # Although it's possible to open an index on some partial
            # list of columns, we'll keep it simple here, and always
            # use all columns.
            full_idxname = 'index:' + tc.tablename + ':' + idx.idxname
            cols = '(' + ','.join([('c' + str(x)) for x in range(tc.nkeys, tc.nvalues + tc.nkeys)]) + ')'
            self.show_python('# check_entries: ' + full_idxname + cols)
            self.show_python("cursor = self.session.open_cursor('" + full_idxname + cols + "', None, None)")
            cursor = self.session.open_cursor(full_idxname + cols, None, None)
            count = 0
            for x in cursor:
                count += 1
            self.assertEqual(count, tc.nentries)
            for i in range(0, tc.nentries):
                key = idx.gen_keys(i)
                val = tc.gen_values(i)
                self.check_one(full_idxname, cursor, key, val)
            cursor.close()
            self.show_python("cursor.close()")
if __name__ == '__main__':
    wttest.run()

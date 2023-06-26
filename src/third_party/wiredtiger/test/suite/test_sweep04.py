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
# test_sweep04.py
# Test lots of tables with more steadily created and dropped.
# A core group of tables is used most often and hangs around.
# Test that the total number of dhandles, while increasing,
# starts to level off, that is, the sweeps are keeping up.
# Then test that if we only access the core tables for a while,
# the total number of dhandles comes back down to a small number.

import time
from suite_random import suite_random
from wiredtiger import stat
import wttest

# Given a set of values corresponding to successive times,
# we have an implied set of points in two dimensions.
# Compute the average for the values and the slope for the
# least squares regression of the line.
#
# We'd use numpy, but we aren't assured that it is always installed
# for our python.
def average_slope(y):
    n = len(y)
    if n == 0:
        return [0, 0]  # there's no average or slope
    elif n == 1:
        return [y[0], 0]  # there's no slope

    average = sum(y) / n
    x = range(1, n + 1)  # The implied x axis, steadily increasing integers.

    # Here's a formula for least squares slope:
    #   https://www.mathsisfun.com/data/least-squares-regression.html
    top = n * sum(x[i] * y[i] for i in range(n)) - sum(x) * sum(y)
    bottom = n * sum(x[i]**2 for i in range(n)) - sum(x)**2
    slope = top / bottom
    return [average, slope]

@wttest.longtest("lots of files")
class test_sweep04(wttest.WiredTigerTestCase):
    tablebase = 'test_sweep04'
    uri = 'table:' + tablebase

    # Configuration values for the run. If any of these values are changed,
    # the simulation will run, but the acceptance criteria may fail at the end.
    core_tables = 10               # Number of core tables that always exist.
    transient_tables = 100         # Number of transient tables at any time.
    transient_examined = 10        # Number of transient tables opened at a time.
    ratio_examined = 0.01          # The chance that transient tables are examined.
    transient_table_max = 10000    # Defines the length of the run.
    numkv = 1                      # Number of k/v pairs. Shouldn't matter for this test.
    nsessions = 100                # Number of sessions in our pool.

    conn_config = 'file_manager=(close_handle_minimum=0,' + \
                  'close_idle_time=3,close_scan_interval=1),' + \
                  'statistics=(fast),operation_tracking=(enabled=false),'

    create_params = 'key_format=i,value_format=i'

    # Create a uri for one of the core tables
    def core_uri(self, i):
        return '%s-c.%d' % (self.uri, i)

    # Create a uri for one of the transient tables
    def transient_uri(self, i):
        return '%s-t.%d' % (self.uri, i)

    def create_table(self, uri):
        self.session.create(uri, self.create_params)
        c = self.session.open_cursor(uri, None)
        for k in range(self.numkv):
            c[k+1] = 1
        c.close()

    # Access a set of tables in some minimal way that ensures its
    # dhandle is at least momentarily in use.  "uri_maker" is a
    # function that is used to create the uri.
    def examine(self, session, uri_maker, start, count):
        for i in range(0, count):
            c = session.open_cursor(uri_maker(start + i))
            self.assertEquals(c[1], 1)
            c.close()

    def test_big_run(self):
        # populate
        r = suite_random()

        # Create the set of core tables
        for i in range(0, self.core_tables):
            self.create_table(self.core_uri(i))

        created_transient = 0    # Running total of transients created.
        available_transient = 0  # The next available transient number.

        # Create the initial batch of transient tables.
        while created_transient < self.transient_tables:
            self.create_table(self.transient_uri(created_transient))
            created_transient += 1

        # Open all the session we'll use in advance.
        sessions = []
        for i in range(self.nsessions):
            sessions.append(self.conn.open_session())

        # We keep the dhandle counts for each time we get stats.
        dhandle_counts = []

        # The big loop: For half the run, we are stressing by accessing both core tables
        # and transient tables, and creating/dropping transient tables. The second half
        # of the run, we stop creating/dropping tables and only access core tables
        # just to see if all outstanding dhandles are swept.
        maxloop = self.transient_table_max * 2
        lasttime = time.time()
        for loopcount in range(0, maxloop):
            stressing = (created_transient < self.transient_table_max)
            if loopcount % 100 == 0:
                self.pr('{}/{}  stressing={}'.format(loopcount, maxloop, stressing))

                # Make sure at least 3 seconds elapses between each 100 times through
                # the loop, to give the various sweeps time to operate.
                thistime = time.time()
                delta = thistime - lasttime
                if delta < 3.0:
                    time.sleep(3.0 - delta)
                lasttime = thistime

            if stressing:
                self.session.drop(self.transient_uri(available_transient), "force")
                available_transient += 1
                self.create_table(self.transient_uri(created_transient))
                created_transient += 1

            rand_session = sessions[r.rand_range(0, self.nsessions)]

            # In the stress part of the run, some small fraction (given by ratio_examined) will
            # look at the transient tables.  Looking at these rarely makes them candidates for
            # closing by the connection sweep.
            big = 1000000   # Any large number works.
            if stressing and r.rand32() % big > big * self.ratio_examined:
                # Access "count" transient tables, starting at table "tnum".
                count = self.transient_examined
                tnum = r.rand_range(available_transient, available_transient + self.transient_tables - count)
                self.examine(rand_session, self.transient_uri, tnum, count)
            else:
                # Access a single core table, numbered "tnum".
                tnum = r.rand_range(0, self.core_tables)
                self.examine(rand_session, self.core_uri, tnum, 1)

            if loopcount % 100 == 99:
                # Gather statistics about the number of dhandles, to be checked at
                # the end of the run.
                stat_cursor = self.session.open_cursor('statistics:', None, None)

                # Enable for detailed output.
                if False:
                    close = stat_cursor[stat.conn.dh_sweep_close][2]
                    remove = stat_cursor[stat.conn.dh_sweep_remove][2]
                    sweep = stat_cursor[stat.conn.dh_sweeps][2]
                    sclose = stat_cursor[stat.conn.dh_session_handles][2]
                    ssweep = stat_cursor[stat.conn.dh_session_sweeps][2]
                    tod = stat_cursor[stat.conn.dh_sweep_tod][2]
                    ref = stat_cursor[stat.conn.dh_sweep_ref][2]
                    self.pr(('DHANDLE STATS: close={}, remove={}, sweep={}, session_handles={}, '+
                             'session_sweeps={}, sweep_tod={}, sweep_ref={}').format(
                                 close, remove, sweep, sclose, ssweep, tod, ref))

                dhandles = stat_cursor[stat.conn.dh_conn_handle_count][2]
                files_open = stat_cursor[stat.conn.file_open][2]
                self.pr('  dhandle_count={}'.format(dhandles))
                self.pr('  file_open={}'.format(files_open))
                dhandle_counts.append(dhandles)
                stat_cursor.close()

                # This (extremely verbose) debugging is disabled, as it writes to stdout,
                # and that causes the test framework fail the test.
                if False:
                    if loopcount % 1000 == 999:
                        self.conn.debug_info('handles=true')
                self.pr('  average,slope={}'.format(str(average_slope(dhandle_counts))))

            # Reset any sessions we've used. This may be necessary to trigger some session sweeps.
            rand_session.reset()
            self.session.reset()

        # The run is finished, process and check the dhandle counts we've collected.
        #
        # Half the run is dhandle growth, and the second half should see a decline.
        # If everything is working right in the first half of the run, we should start
        # to see the number of dhandles start to reach an asymptote. So we check the slope of
        # the dhandle line in the second quarter to see that is the case.  In the second
        # half of the run, where we aren't accessing the transient tables, those references
        # should free up, and we should see an asymptote at the end much closer to the
        # number of files.
        half = len(dhandle_counts)//2
        qtr = len(dhandle_counts)//4
        tenth = len(dhandle_counts)//10

        (q1_avg, q1_slope) = average_slope(dhandle_counts[0:qtr])
        (q2_avg, q2_slope) = average_slope(dhandle_counts[qtr:half])
        (q3_avg, q3_slope) = average_slope(dhandle_counts[half:-qtr])
        (q4_avg, q4_slope) = average_slope(dhandle_counts[-qtr:])
        (end_run_avg, end_run_slope) = average_slope(dhandle_counts[-tenth:])

        self.pr('1st qtr:  average={},slope={}'.format(q1_avg, q1_slope))
        self.pr('2nd qtr:  average={},slope={}'.format(q2_avg, q2_slope))
        self.pr('3rd qtr:  average={},slope={}'.format(q3_avg, q3_slope))
        self.pr('4th qtr:  average={},slope={}'.format(q4_avg, q4_slope))

        self.pr('end run: average={},slope={}'.format(end_run_avg, end_run_slope))

        # Note, we don't check the first half average, it's likely to be big, but its size
        # depends on many factors. The important thing is that the slope has flattened out.
        # Even with variations due to sweep timing, the slope shouldn't be greater than
        # 15.0 (dhandles per 100 times through the loop).
        self.assertLess(abs(q2_slope), q1_slope)
        self.assertLess(abs(q2_slope), 15.0)

        # At the end of the run, we expect a pretty flat slope and a pretty small number
        # of dhandles. A slope of 5.0 (dhandles per 100 times though the loop) is rather
        # flat and still leaves room for some variation.
        self.assertLess(abs(end_run_slope), 5.0)
        self.assertLess(end_run_avg, self.core_tables + self.transient_tables + 20)

if __name__ == '__main__':
    wttest.run()

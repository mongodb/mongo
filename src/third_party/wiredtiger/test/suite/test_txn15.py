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
# test_txn15.py
#   Transactions: different sync modes
#

import fnmatch, os, shutil, time
from suite_subprocess import suite_subprocess
from wiredtiger import stat
from wtscenario import make_scenarios
import wttest

class test_txn15(wttest.WiredTigerTestCase, suite_subprocess):
    uri = 'table:test_txn15_1'
    entries = 100
    # Turn on logging for this test.
    def conn_config(self):
        return 'statistics=(fast),' + \
            'log=(archive=false,enabled,file_max=100K),' + \
            'use_environment=false,' + \
            'transaction_sync=(enabled=%s),' % self.conn_enable + \
            'transaction_sync=(method=%s),' % self.conn_method

    format_values = [
        ('integer-row', dict(key_format='i', value_format='i')),
        ('column', dict(key_format='r', value_format='i')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    conn_sync_enabled = [
        ('en_off', dict(conn_enable='false')),
        ('en_on', dict(conn_enable='true')),
    ]
    conn_sync_method = [
        ('m_dsync', dict(conn_method='dsync')),
        ('m_fsync', dict(conn_method='fsync')),
        ('m_none', dict(conn_method='none')),
    ]
    begin_sync = [
        ('b_method', dict(begin_sync='sync=true')),
        ('b_none', dict(begin_sync=None)),
        ('b_off', dict(begin_sync='sync=false')),
    ]
    # We don't test 'background' in this test.  It isn't applicable for the
    # particulars here and reduces the number of scenarios.  It is an error for
    # both begin_transaction and commit_transaction to have explicit settings.
    # We will avoid that in the code below.
    commit_sync = [
        ('c_method', dict(commit_sync='sync=on')),
        ('c_none', dict(commit_sync=None)),
        ('c_off', dict(commit_sync='sync=off')),
    ]
    scenarios = make_scenarios(format_values, conn_sync_enabled, conn_sync_method,
        begin_sync, commit_sync)

    def mkvalue(self, i):
        if self.value_format == '8t':
            return i % 256
        return i

    # Given the different configuration settings determine if this group
    # of settings would result in either a wait for write or sync.
    # Returns None, "write" or "sync".  None means no waiting for either.
    # "write" means wait for write, but not sync.  "sync" means both
    # write and sync.
    def syncLevel(self):
        # We know that we skip illegal settings where both begin and commit
        # have explicit settings.  So we don't have to check that here.

        # If the transaction doesn't override it, then it is whatever is
        # set on the connection.
        if self.begin_sync == None and self.commit_sync == None:
            if self.conn_enable == 'false':
                return None
            # Only fsync implies sync.  Both dsync and none (meaning
            # write-no-sync) imply write.
            if self.conn_method == 'fsync':
                return 'sync'
            else:
                return 'write'
        # If the user explicitly turns it off for the transaction, it is None.
        if self.begin_sync == 'sync=false' or self.commit_sync == 'sync=off':
            return None

        # If we get here, the transaction turns sync on in some way so we
        # only need to check the method type.
        if self.conn_method == 'fsync':
            return 'sync'
        else:
            return 'write'

    def test_sync_ops(self):
        # It is illegal to set a sync option on both begin and commit.
        # If the scenario multiplication sets both, just return.
        if self.begin_sync != None and self.commit_sync != None:
            return

        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.uri, create_params)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        #
        # !!! Implementation detail here.  We record the stats that
        # indicate the code went through the release code explicitly
        # (instead of the worker thread).  They indicate we came
        # through the special case path for handling a flush or sync.
        #
        write1 = stat_cursor[stat.conn.log_release_write_lsn][2]
        sync1 = stat_cursor[stat.conn.log_sync][2]
        stat_cursor.close()

        c = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction(self.begin_sync)
        for i in range(1, self.entries + 1):
            c[i] = self.mkvalue(i + 1)
        self.session.commit_transaction(self.commit_sync)
        c.close()

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        write2 = stat_cursor[stat.conn.log_release_write_lsn][2]
        sync2 = stat_cursor[stat.conn.log_sync][2]
        stat_cursor.close()

        checks = self.syncLevel()
        # Checking sync implies the write.
        # If we did special processing the stats should not be the
        # same.  Otherwise the number of writes should be equal.
        # We do not check for syncs being equal because worker threads
        # also increment that stat when they sync and close a log file.
        if checks != None:
            self.assertNotEqual(write1, write2)
            if checks == 'sync':
                self.assertNotEqual(sync1, sync2)
        else:
            self.assertEqual(write1, write2)

if __name__ == '__main__':
    wttest.run()

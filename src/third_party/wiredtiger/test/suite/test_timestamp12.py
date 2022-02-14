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
# test_timestamp12.py
#   Timestamps: Test the use_timestamp setting when closing the connection.
#

import wttest
from wtscenario import make_scenarios

class test_timestamp12(wttest.WiredTigerTestCase):
    conn_config = 'config_base=false,create,log=(enabled)'
    coll_uri = 'table:collection12'
    oplog_uri = 'table:oplog12'

    format_values = [
        ('integer-row', dict(key_format='i', value_format='i')),
        ('column', dict(key_format='r', value_format='i')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    closecfg = [
        ('dfl', dict(close_cfg='', all_expected=False)),
        ('use_stable', dict(close_cfg='use_timestamp=true', all_expected=False)),
        ('all_dirty', dict(close_cfg='use_timestamp=false', all_expected=True)),
    ]
    scenarios = make_scenarios(format_values, closecfg)

    def verify_expected(self, op_exp, coll_exp):
        c_op = self.session.open_cursor(self.oplog_uri)
        c_coll = self.session.open_cursor(self.coll_uri)
        op_actual = dict((k, v) for k, v in c_op if v != 0)
        coll_actual = dict((k, v) for k, v in c_coll if v != 0)
        #print "CHECK: Op Expected"
        #print op_exp
        #print "CHECK: Op Actual"
        #print op_actual
        self.assertTrue(op_actual == op_exp)
        #print "CHECK: Coll Expected"
        #print coll_exp
        #print "CHECK: Coll Actual"
        #print coll_actual
        self.assertTrue(coll_actual == coll_exp)

    def test_timestamp_recovery(self):
        #
        # Create several collection-like tables that are checkpoint durability.
        # Add data to each of them separately and checkpoint so that each one
        # has a different stable timestamp.
        #
        basecfg = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.oplog_uri, basecfg)
        self.session.create(self.coll_uri, basecfg + ',log=(enabled=false)')
        c_op = self.session.open_cursor(self.oplog_uri)
        c_coll = self.session.open_cursor(self.coll_uri)

        # Begin by adding some data.
        nentries = 10
        first_range = range(1, nentries)
        second_range = range(nentries, nentries*2)
        all_keys = range(1, nentries*2)
        for i in first_range:
            self.session.begin_transaction()
            c_op[i] = 1
            c_coll[i] = 1
            self.session.commit_transaction(
              'commit_timestamp=' + self.timestamp_str(i))
        # Set the oldest and stable timestamp to the end.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(nentries-1) +
        ',stable_timestamp=' + self.timestamp_str(nentries-1))

        # Add more data but don't advance the stable timestamp.
        for i in second_range:
            self.session.begin_transaction()
            c_op[i] = 1
            c_coll[i] = 1
            self.pr("i: " + str(i))
            self.session.commit_transaction(
              'commit_timestamp=' + self.timestamp_str(i))

        # Close and reopen the connection. We cannot use reopen_conn because
        # we want to test the specific close configuration string.
        self.close_conn(self.close_cfg)
        self.open_conn()

        # Set up our expected data and verify after the reopen.
        op_exp = dict((k, 1) for k in all_keys)
        if self.all_expected == True:
            coll_exp = dict((k, 1) for k in all_keys)
        else:
            coll_exp = dict((k, 1) for k in first_range)

        self.verify_expected(op_exp, coll_exp)

if __name__ == '__main__':
    wttest.run()

#!/usr/bin/env python
#
# Public Domain 2014-2019 MongoDB, Inc.
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
# test_prepare06.py
#   Prepare: Rounding up prepared transactions Timestamps.
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest

def timestamp_str(t):
    return '%x' % t

class test_prepare06(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_prepare06'
    uri = 'table:' + tablename
    session_config = 'isolation=snapshot'

    def test_timestamp_api(self):
        self.session.create(self.uri, 'key_format=i,value_format=i')
        c = self.session.open_cursor(self.uri)

        # It is illegal to set the prepare timestamp older than the oldest
        # timestamp.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(20))
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(30))
        self.session.begin_transaction()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.prepare_transaction(
            'prepare_timestamp=' + timestamp_str(10)),
            "/older than the oldest timestamp/")
        self.session.rollback_transaction()

        # Check setting a prepared transaction timestamps earlier than the
        # oldest timestamp is valid with roundup_timestamps settings.
        self.session.begin_transaction('roundup_timestamps=(prepared=true)')
        self.session.prepare_transaction('prepare_timestamp=' + timestamp_str(10))
        self.session.timestamp_transaction('commit_timestamp=' + timestamp_str(15))
        self.session.timestamp_transaction('durable_timestamp=' + timestamp_str(35))
        self.session.commit_transaction()

        # Check setting a prepared transaction timestamps earlier than the
        # oldest timestamp is invalid, if durable timestamp is less than the
        # stable timestamp.
        self.session.begin_transaction('roundup_timestamps=(prepared=true)')
        self.session.prepare_transaction('prepare_timestamp=' + timestamp_str(10))
        self.session.timestamp_transaction('commit_timestamp=' + timestamp_str(15))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.timestamp_transaction(
            'durable_timestamp=' + timestamp_str(25)),
            "/is less than the stable timestamp/")
        self.session.rollback_transaction()

        # Check the cases with an active reader.
        # Start a new reader to have an active read timestamp.
        s_reader = self.conn.open_session()
        s_reader.begin_transaction('read_timestamp=' + timestamp_str(40))

        # It is illegal to set the prepare timestamp as earlier than an active
        # read timestamp even with roundup_timestamps settings.  This is only
        # checked in diagnostic builds.
        if wiredtiger.diagnostic_build():
            self.session.begin_transaction('roundup_timestamps=(prepared=true)')
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.prepare_transaction(
                'prepare_timestamp=' + timestamp_str(10)),
                "/must be greater than the latest active read timestamp/")
            self.session.rollback_transaction()

            # It is illegal to set the prepare timestamp the same as an active read
            # timestamp even with roundup_timestamps settings.
            self.session.begin_transaction('roundup_timestamps=(prepared=true)')
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.prepare_transaction(
                'prepare_timestamp=' + timestamp_str(40)),
                "/must be greater than the latest active read timestamp/")
            self.session.rollback_transaction()

        # It is illegal to set a commit timestamp less than the prepare
        # timestamp of a transaction.
        self.session.begin_transaction()
        c[1] = 1
        self.session.prepare_transaction(
            'prepare_timestamp=' + timestamp_str(45))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(
            'commit_timestamp=' + timestamp_str(30)),
            "/less than the prepare timestamp/")

        # It is legal to set a commit timestamp older than prepare timestamp of
        # a transaction with roundup_timestamps settings.
        self.session.begin_transaction('roundup_timestamps=(prepared=true)')
        c[1] = 1
        self.session.prepare_transaction(
            'prepare_timestamp=' + timestamp_str(45))
        self.session.timestamp_transaction('commit_timestamp=' + timestamp_str(30))
        #self.session.timestamp_transaction('durable_timestamp=' + timestamp_str(30))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.timestamp_transaction(
            'durable_timestamp=' + timestamp_str(30)),
            "/is less than the commit timestamp/")
        self.session.rollback_transaction()

        s_reader.commit_transaction()

if __name__ == '__main__':
    wttest.run()

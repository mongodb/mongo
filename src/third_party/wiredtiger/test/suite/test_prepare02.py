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
# test_prepare02.py
#   Prepare : Session API usage generates expected error in prepared state.
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest

def timestamp_str(t):
    return '%x' % t

class test_prepare02(wttest.WiredTigerTestCase, suite_subprocess):
    def test_prepare_session_operations(self):
        self.session.create("table:mytable", "key_format=S,value_format=S")
        cursor = self.session.open_cursor("table:mytable", None)

        # Test the session methods that are forbidden after the transaction is
        # prepared.
        self.session.begin_transaction()
        self.session.prepare_transaction("prepare_timestamp=2a")
        msg = "/ not permitted in a/"
        #
        # The operations listed below are not supported in the prepared state.
        #
        # The operations are listed in the same order as they are declared in
        # the session structure. Any function missing below is allowed in the
        # prepared state.
        #
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.reconfigure(), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.alter("table:mytable",
                "access_pattern_hint=random"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create("table:mytable1",
                "key_format=S,value_format=S"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.compact("table:mytable"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.drop("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(cursor, cursor,
                "compare=gt,count=10"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.log_flush("sync=on"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.log_printf("Printing to log file"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.rebalance("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.rename("table:mytable", "table:mynewtable",
                None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.reset(), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.salvage("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate("table:mytable",
                None, None, None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.upgrade("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.verify("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.begin_transaction(), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.prepare_transaction("prepare_timestamp=2a"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.timestamp_transaction(
                "read_timestamp=2a"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.checkpoint(), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.snapshot("name=test"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.transaction_sync(), msg)
        self.session.rollback_transaction()

        # Commit after prepare is permitted.
        self.session.begin_transaction()
        c1 = self.session.open_cursor("table:mytable", None)
        self.session.prepare_transaction("prepare_timestamp=2a")
        self.session.commit_transaction("commit_timestamp=2b")

        # Setting commit timestamp via timestamp_transaction after
        # prepare is also permitted.
        self.session.begin_transaction()
        c1 = self.session.open_cursor("table:mytable", None)
        self.session.prepare_transaction("prepare_timestamp=2a")
        self.session.timestamp_transaction("commit_timestamp=2b")
        self.session.commit_transaction()

        # Rollback after prepare is permitted.
        self.session.begin_transaction()
        self.session.prepare_transaction("prepare_timestamp=2a")
        self.session.rollback_transaction()

        # Close after prepare is permitted.
        self.session.begin_transaction()
        self.session.prepare_transaction("prepare_timestamp=2a")
        self.session.close()

if __name__ == '__main__':
    wttest.run()

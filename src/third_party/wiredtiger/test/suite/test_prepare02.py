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

class test_prepare02(wttest.WiredTigerTestCase, suite_subprocess):

    def test_prepare_session_operations(self):

        # Test the session methods forbidden after the transaction is prepared.
        self.session.create("table:mytable", "key_format=S,value_format=S")
        self.session.begin_transaction()
        cursor = self.session.open_cursor("table:mytable", None)
        cursor["key"] = "value"
        self.session.prepare_transaction("prepare_timestamp=2a")
        msg = "/not permitted in a prepared transaction/"

        # The operations are listed in the same order as they are declared in the session structure.
        # WT_SESSION.close permitted.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.reconfigure(), msg)
        # WT_SESSION.strerror permitted, but currently broken in the Python API (WT-5399).
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.alter("table:mytable", "access_pattern_hint=random"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create("table:mytable1", "key_format=S,value_format=S"), msg)
        # WT_SESSION.import permitted, not supported in the Python API.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.compact("table:mytable"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.drop("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(cursor, cursor, "compare=gt,count=10"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.log_flush("sync=on"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.rename("table:mytable", "table:mynewtable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.reset(), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.salvage("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.truncate("table:mytable", None, None, None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.upgrade("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.verify("table:mytable", None), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.begin_transaction(), msg)
        # WT_SESSION.commit_transaction permitted, tested below.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.prepare_transaction("prepare_timestamp=2a"), msg)
        # WT_SESSION.rollback_transaction permitted, tested below.
        self.session.timestamp_transaction("commit_timestamp=2b")
        self.assertTimestampsEqual(self.session.query_timestamp('get=prepare'), '2a')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.checkpoint(), msg)
        self.session.breakpoint()

        # Commit the transaction. Test that no "not permitted in a prepared transaction" error has
        # set a transaction error flag, that is, we should still be able to commit successfully.
        self.session.timestamp_transaction("commit_timestamp=2b")
        self.session.timestamp_transaction("durable_timestamp=2b")
        self.session.commit_transaction('commit_timestamp=2a')

        # Commit after prepare is permitted.
        self.session.begin_transaction()
        c1 = self.session.open_cursor("table:mytable", None)
        self.session.prepare_transaction("prepare_timestamp=2a")
        self.session.timestamp_transaction("commit_timestamp=2b")
        self.session.timestamp_transaction("durable_timestamp=2b")
        self.session.commit_transaction()

        # Setting commit timestamp via timestamp_transaction after prepare is also permitted.
        self.session.begin_transaction()
        c1 = self.session.open_cursor("table:mytable", None)
        self.session.prepare_transaction("prepare_timestamp=2a")
        self.session.timestamp_transaction("commit_timestamp=2b")
        self.session.timestamp_transaction("durable_timestamp=2b")
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

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
import wttest
from helper import copy_wiredtiger_home
from prepare_util import test_prepare_preserve_prepare_base

# Test database without the preserve prepared config can read data written by
# the database with the preserve prepared config.

class test_prepare38(test_prepare_preserve_prepare_base):
    uri = 'table:test_prepare38'

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_open(self):
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = "value1"
        self.session.commit_transaction()

        self.session.begin_transaction()
        # Do a prepared remove
        cursor.set_key(1)
        cursor.remove()
        # Do a prepared update
        cursor[2] = "value2"
        # Do a prepared update remove
        cursor[3] = "value3"
        cursor.set_key(3)
        cursor.remove()
        self.session.prepare_transaction(f"prepare_timestamp={self.timestamp_str(10)},prepared_id={self.prepared_id_str(1)}")

        # Make the prepared timestamp stable
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        session2 = self.conn.open_session()
        # Write the prepared updates to disk
        session2.checkpoint()

        copy_wiredtiger_home(self, ".", "RESTART")
        # Reopen without the preserve prepared config
        conn = self.wiredtiger_open("RESTART", "preserve_prepared=false")
        conn.close()

        self.session.rollback_transaction(f"rollback_timestamp={self.timestamp_str(30)}")
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

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
import wiredtiger

# test_prepare19.py
# Test that for in-memory configurations of WiredTiger if rolling back a prepared, reconciled 
# update results in an empty update chain then a tombstone is appended to the chain
class test_prepare19(wttest.WiredTigerTestCase):

    def conn_config(self):
        return 'in_memory=true'

    def test_server_example(self):
        uri = 'table:test_prepare19'
        config = 'key_format=i,value_format=S,log=(enabled=false)'

        self.session.create(uri, config)

        # Place more than 1000 aborted updates on the update chain. 
        for i in range(1, 1100):
            self.session.begin_transaction()
            cursor = self.session.open_cursor(uri, None)
            cursor[1] = ""
            self.session.rollback_transaction()

        # Make a prepared update on key 1, force eviction, and rollback.
        self.prepare_evict_rollback(uri, config, 1101)

        # If no tombstone is written the update will be aborted in the update chain but not in the
        # btree.
        # The transaction will see an active transaction on key 1 and raise a write conflict.
        # Expect no error is raised.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri, None)
        cursor[1] = ""


    def prepare_evict_rollback(self, uri, config, timestamp):
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri, None)
        cursor[1] = ""
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(timestamp))

        # This write conflict on the same page as key 1 results in a forced 
        # eviction when the key has more than 1000 updates in its update chain.
        write_conflict_session = self.conn.open_session()
        write_conflict_session.create(uri, config)
        write_conflict_session.begin_transaction()
        write_conflict_cursor = write_conflict_session.open_cursor(uri, None)
        try:
            write_conflict_cursor[1] = "",
            raise Exception
        except wiredtiger.WiredTigerError:
            pass
        write_conflict_session.rollback_transaction()
        write_conflict_session.close()

        self.session.rollback_transaction()


if __name__ == '__main__':
    wttest.run()

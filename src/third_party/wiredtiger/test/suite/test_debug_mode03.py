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

import wiredtiger, wttest

# test_debug_mode03.py
#    Test the debug mode settings. Test table_logging use.
class test_debug_mode03(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled=true,file_max=100K),debug_mode=(table_logging=true)'
    uri = 'file:test_debug'
    entries = 100
    value = b'\x01\x02abcd\x03\x04'

    def add_data(self):
        # Add a binary value we can search for in the log.
        keys = range(0, self.entries)
        c = self.session.open_cursor(self.uri, None)
        for k in keys:
            c[k] = self.value
        c.close()

    def find_log_recs(self):
        # Open a log cursor. We should find log records that have
        # the value we inserted.
        c = self.session.open_cursor("log:", None)
        count = 0
        while c.next() == 0:
            # lsn.file, lsn.offset, opcount
            keys = c.get_key()
            # txnid, rectype, optype, fileid, logrec_key, logrec_value
            values = c.get_value()
            # Look for log records that have a key/value pair.
            if values[4] != b'':
                if self.value in values[5]:  # logrec_value
                    count += 1
        c.close()
        return count

    def test_table_logging(self):
        self.session.create(self.uri, 'key_format=i,value_format=u,log=(enabled=false)')
        self.add_data()
        count = self.find_log_recs()
        self.assertEqual(count, self.entries)

    # Test that both zero and one archive as usual. And test reconfigure.
    def test_table_logging_off(self):
        self.conn.reconfigure("debug_mode=(table_logging=false)")
        self.session.create(self.uri, 'key_format=i,value_format=u,log=(enabled=false)')
        self.add_data()
        count = self.find_log_recs()
        self.assertEqual(count, 0)

if __name__ == '__main__':
    wttest.run()

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

import struct, wiredtiger, wttest

def timestamp(kind, ts):
    return "{}_timestamp={:X}".format(kind, ts)

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

    def add_data_at_ts(self, ts):
        self.session.begin_transaction()
        self.add_data()
        self.session.commit_transaction(timestamp("commit", ts))

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

    def pack_large_int(self, large):
        # This line in intpacking.py tells us how to pack a large integer:
        #
        #    First byte | Next bytes | Min Value        | Max Value
        #   [11 10llll] | l          | 2^13 + 2^6       | 2^64 - 1
        #
        # An 8 byte integer is packed as byte 0xe8, followed by 8 bytes packed for big endian.
        LARGE_INT = 2**13 + 2**6
        self.assertGreaterEqual(large, LARGE_INT)
        packed_int = b'\xe8' + struct.pack('>Q', large - LARGE_INT)   # >Q == 8 bytes big endian
        return packed_int

    def find_ts_log_rec(self, ts):
        # The timestamp we're looking for will be encoded as a 'packed integer' in the log file.  We
        # don't need a general purpose encoder here, we just need to know how to pack large integers.
        packed_int = self.pack_large_int(ts)

        # Open a log cursor, and we look for the timestamp somewhere in the values.
        c = self.session.open_cursor("log:", None)
        count = 0
        while c.next() == 0:
            # lsn.file, lsn.offset, opcount
            keys = c.get_key()
            # txnid, rectype, optype, fileid, logrec_key, logrec_value
            values = c.get_value()

            #self.tty('LOG: keys={}, values={}\n    val5={}\n    packed={}'.format(
            #    str(keys), str(values), values[5].hex(), packed_int.hex()))
            if packed_int in values[5]:  # logrec_value
                count += 1
        c.close()
        return count

    def test_table_logging(self):
        self.session.create(self.uri, 'key_format=i,value_format=u,log=(enabled=false)')
        self.add_data()
        count = self.find_log_recs()
        self.assertEqual(count, self.entries)

    # Test that both zero and one remove as usual. And test reconfigure.
    def test_table_logging_off(self):
        self.conn.reconfigure("debug_mode=(table_logging=false)")
        self.session.create(self.uri, 'key_format=i,value_format=u,log=(enabled=false)')
        self.add_data()
        count = self.find_log_recs()
        self.assertEqual(count, 0)

    # Debug table logging with operations from timestamp_transaction
    def test_table_logging_ts(self):
        # We pick a large timestamp because encoding a large integer is relatively easy
        # and we won't get any false positives when searching the log file.
        base_ts = 0x1020304050600000

        self.session.create(self.uri, 'key_format=i,value_format=u,log=(enabled=false)')
        self.add_data_at_ts(base_ts + 0x100)

        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri, None)
        c[self.entries] = self.value
        c.close()
        self.session.timestamp_transaction(timestamp("read", base_ts + 0x200))
        self.session.prepare_transaction(timestamp("prepare", base_ts + 0x201))
        self.session.timestamp_transaction(timestamp("commit", base_ts + 0x202))
        self.session.timestamp_transaction(timestamp("durable", base_ts + 0x203))
        self.session.commit_transaction()

        self.assertGreater(self.find_ts_log_rec(base_ts + 0x100), 0)
        self.assertGreater(self.find_ts_log_rec(base_ts + 0x200), 0)
        self.assertGreater(self.find_ts_log_rec(base_ts + 0x201), 0)
        self.assertGreater(self.find_ts_log_rec(base_ts + 0x202), 0)
        self.assertGreater(self.find_ts_log_rec(base_ts + 0x203), 0)

if __name__ == '__main__':
    wttest.run()

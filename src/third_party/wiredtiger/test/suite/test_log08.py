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

import struct
import wttest

# test_log08.py
#   A log cursor that salvages a corrupt record at the end of the log must end
#   with WT_NOTFOUND, not an error. Recovery walks the log with this cursor at
#   open, so an error fails the whole open.
class test_log08(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled,file_max=100K,zero_fill=false)'
    uri = 'table:test_log08'

    def test_log_cursor_after_salvage(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'value'
        cursor.close()
        self.session.log_flush('sync=on')

        # Records are 128-byte slots whose first 4 bytes are the length.
        # Walk until that length is 0 (unused preallocated tail) and write
        # 0x08, which is too small to be a real header. The log cursor must
        # salvage that leftover and stop, not return an error.
        with open('WiredTigerLog.0000000001', 'r+b') as f:
            offset = 0
            while True:
                f.seek(offset)
                header = f.read(4)
                if len(header) < 4:
                    break
                rec_len = struct.unpack('<I', header)[0]
                if rec_len == 0:
                    break
                offset += (rec_len + 127) // 128 * 128
            f.seek(offset)
            f.write(b'\x08')

        log_cursor = self.session.open_cursor('log:')
        with self.expectedStdoutPattern('record len corruption 0x8'):
            while log_cursor.next() == 0:
                pass
        log_cursor.close()

        self.reopen_conn()
        cursor = self.session.open_cursor(self.uri)
        self.assertEqual(cursor[1], 'value')
        cursor.close()

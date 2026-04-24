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

# test_log06.py
#   Reproduce a partial-log-record recovery scenario and verify it succeeds.
#
#   A pre-allocated log file can end up with a 128-byte-aligned block where
#   len==0 but other bytes are non-zero, because a log write was still being
#   flushed when the crash happened. __log_has_hole + __log_record_verify
#   detect it, emit a message, truncate the log, and recovery
#   continues. Committed data before the block must survive intact.
#
#   Two scenarios, per which header byte is non-zero:
#       record_len: bytes 4-7   -> "record len corruption 0x0"
#       flag:       bytes 8-9   -> "flag corruption 0x4"

import os
import wttest
from helper import copy_wiredtiger_home
from wtscenario import make_scenarios


class test_log06(wttest.WiredTigerTestCase):
    # Write to the OS but skip fsync, so a crash can leave partial writes.
    conn_config = 'log=(enabled),transaction_sync=(enabled=true,method=none)'

    uri = 'table:log06'
    nrows = 100

    # 128-byte pseudo-record (WT_LOG_ALIGN). bytes[0:4]==0 always fires
    # "record len corruption 0x0"; the non-zero byte makes __log_has_hole
    # notice the stride and, in the flag scenario, drives the second NOTICE.
    scenarios = make_scenarios([
        ('record_len',
         dict(block=b'\x00\x00\x00\x00\xde\xad\xbe\xef' + b'\x00' * 120,
              expect='record len corruption 0x0')),
        ('flag',
         # bytes[8:10] = flags field, little-endian 0x0004 (unknown bit 2).
         dict(block=b'\x00' * 8 + b'\x04\x00' + b'\x00' * 118,
              expect='flag corruption 0x4')),
    ])

    def test_recovery_from_partial_log_record(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        # Phase 1: value_a is durable via the checkpoint.
        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            cursor[i] = 'value_a'
        self.session.commit_transaction()
        self.session.checkpoint()

        # Phase 2: value_b is in the WAL but not checkpointed; recovery
        # must replay the log to restore it.
        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            cursor[i] = 'value_b'
        self.session.commit_transaction()
        cursor.close()

        # Phase 3: copy while the connection is still open (no clean-shutdown
        # marker in the copy), then append the scenario's block to every log
        # file so recovery will encounter the partial record.
        copy_wiredtiger_home(self, '.', 'RESTART')
        for fname in os.listdir('RESTART'):
            if fname.startswith('WiredTigerLog.'):
                with open(os.path.join('RESTART', fname), 'ab') as f:
                    f.write(self.block)
        self.close_conn()

        # Phase 4: recovery emits the expected NOTICE, salvages, continues.
        with self.expectedStdoutPattern(self.expect):
            self.conn = self.setUpConnectionOpen('RESTART')
            self.session = self.setUpSessionOpen(self.conn)

        # Phase 5: every key must carry value_b (phase-2 replay succeeded).
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.assertEqual(cursor[i], 'value_b')
        cursor.close()

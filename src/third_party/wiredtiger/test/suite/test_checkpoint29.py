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

from wtscenario import make_scenarios
import wiredtiger, wttest

# test_checkpoint29.py
#
# Test opening a checkpoint cursor after bulk operations.
@wttest.skip_for_hook("disagg", "layered trees do not support named checkpoints")
@wttest.skip_for_hook("tiered", "Fails with tiered storage")
class test_checkpoint(wttest.WiredTigerTestCase):
    ckpt_precision = [
        ('fuzzy', dict(ckpt_config='precise_checkpoint=false')),
        ('precise', dict(ckpt_config='precise_checkpoint=true')),
    ]

    scenarios = make_scenarios(ckpt_precision)

    def conn_config(self):
        return self.ckpt_config

    def test_checkpoint(self):
        # Avoid checkpoint error with precise checkpoint
        if self.ckpt_config == 'precise_checkpoint=true':
            self.conn.set_timestamp('stable_timestamp=1')

        uri = 'table:checkpoint29'
        internal_checkpoint_name = 'WiredTigerCheckpoint'

        # Create an empty table.
        self.session.create(uri, "key_format=S,value_format=S")

        # Ensure that everything is written to the disk.
        self.session.checkpoint()

        # Open a bulk cursor on the table and close it. This will create a single-file checkpoint on
        # the table.
        cursor = self.session.open_cursor(uri, None, "bulk")
        cursor.close()

        # The single-file checkpoint from the bulk cursor cannot be used by the checkpoint cursor,
        # an error should be returned and a warning should be printed out.
        checkpoint_error_msg = 'could not open the checkpoint'
        with self.expectedStdoutPattern(checkpoint_error_msg):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.session.open_cursor(uri, None,
                                                 f"checkpoint={internal_checkpoint_name}"))

        # Perform a system wide checkpoint to remove the existing inconsistency.
        self.session.checkpoint()

        # Open the checkpoint cursor successfully.
        cursor = self.session.open_cursor(uri, None, f"checkpoint={internal_checkpoint_name}")
        cursor.close()

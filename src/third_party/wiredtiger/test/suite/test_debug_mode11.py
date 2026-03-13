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
from wtscenario import make_scenarios
from helper import WiredTigerCursor

# test_debug_mode11.py
#   Verify shutdown checkpoint behavior with close config debug.skip_checkpoint.
class test_debug_mode11(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'
    create_config = 'key_format=S,value_format=S'
    uri = 'table:test_debug_mode11'

    scenarios = make_scenarios([
        ('with_shutdown_checkpoint', dict(skip_shutdown_checkpoint=False, close_cfg="")),
        ('without_shutdown_checkpoint', dict(skip_shutdown_checkpoint=True, close_cfg="debug=(skip_checkpoint=true)")),
    ])

    def verify_key(self, key, exp_value):
        with WiredTigerCursor(self.session, self.uri) as cursor:
            cursor.set_key(key)
            ret = cursor.search()
            if ret == wiredtiger.WT_NOTFOUND:
                self.assertEqual(exp_value, None, f"Expected key {key} to exist but not found")
            else:
                self.assertEqual(ret, 0)
                value = cursor.get_value()
                self.assertEqual(value, exp_value,
                                f"Expected value for key {key} to be {exp_value} but got {value}")

    @wttest.skip_for_hook("tiered", "Fails with tiered storage")
    def test_skip_shutdown_checkpoint_restart_visibility(self):
        # Build a baseline on an explicit checkpoint.
        self.session.create(self.uri, self.create_config)
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['ckpt_1st'] = 'v'
        cursor.close()
        self.session.checkpoint()

        # Write after the last explicit checkpoint. This record is only persisted
        # if shutdown checkpoint runs during close.
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['ckpt_2nd'] = 'v'
        cursor.close()

        # Reopen normally. The only behavior difference should come from
        # whether the shutdown checkpoint ran during close.
        self.close_conn(self.close_cfg)
        self.reopen_conn()

        # Baseline key should always be visible regardless of close config.
        self.verify_key('ckpt_1st', 'v')

        # Behavioral difference is scenario-defined:
        # ckpt_2nd should only be visible when shutdown checkpoint is enabled.
        self.verify_key('ckpt_2nd', None if self.skip_shutdown_checkpoint else 'v')


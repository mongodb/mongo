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

import wttest, wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin
from wtscenario import make_scenarios
from wiredtiger import stat

# Exercise the debug_mode.disagg_slow_truncate_follower connection knob:
# config-parsing smoke checks plus a follower-side truncate that asserts
# the knob actually selects the slow vs fast path.

@disagg_test_class
class test_layered_fast_truncate20(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader"),statistics=(all)'
    uri = f'layered:{test_name}'
    nitems = 500
    trunc_lo, trunc_hi = 100, 400

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # --- config parsing / reconfigure smoke checks ---

    def test_open_accepts_true(self):
        self.reopen_conn(config='debug_mode=(disagg_slow_truncate_follower=true)')

    def test_open_accepts_false(self):
        self.reopen_conn(config='debug_mode=(disagg_slow_truncate_follower=false)')

    def test_open_default(self):
        # Default omits the knob entirely.
        self.reopen_conn(config='')

    def test_reconfigure_toggle(self):
        self.conn.reconfigure('debug_mode=(disagg_slow_truncate_follower=true)')
        self.conn.reconfigure('debug_mode=(disagg_slow_truncate_follower=false)')

    def test_reconfigure_rejects_invalid(self):
        with self.expectedStderrPattern("expected a boolean"):
            self.assertRaisesException(
                wiredtiger.WiredTigerError,
                lambda: self.conn.reconfigure(
                    'debug_mode=(disagg_slow_truncate_follower=bogus)'))

    # --- behavior: slow vs fast follower truncate path ---

    def setup_follower_with_knob(self, slow):
        knob = 'true' if slow else 'false'
        self.reopen_disagg_conn(
            f'disaggregated=(role="follower"),'
            f'debug_mode=(disagg_slow_truncate_follower={knob}),')

    def test_slow_path_calls_cursor_remove_per_key(self):
        self.setup_leader(keys=range(self.nitems))
        self.setup_follower_with_knob(slow=True)

        before = self.get_stat(self.conn, stat.conn.layered_curs_remove)
        self.truncate(self.trunc_lo, self.trunc_hi)
        after = self.get_stat(self.conn, stat.conn.layered_curs_remove)

        expected = self.trunc_hi - self.trunc_lo + 1
        self.assertEqual(after - before, expected,
            f'slow path should call cursor->remove() {expected} times')

    def test_fast_path_skips_cursor_remove(self):
        self.setup_leader(keys=range(self.nitems))
        self.setup_follower_with_knob(slow=False)

        before = self.get_stat(self.conn, stat.conn.layered_curs_remove)
        self.truncate(self.trunc_lo, self.trunc_hi)
        after = self.get_stat(self.conn, stat.conn.layered_curs_remove)

        self.assertEqual(after, before,
            'fast path should not call cursor->remove() (ingest table is empty)')

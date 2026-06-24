#!/usr/bin/env python3
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

# test_layered_stepup12.py
#   Verify that schema operations on layered tables are illegal during a role transition.
#   Attempting to take a schema lock fires an assert when step-up or step-down is ongoing,
#   aborting the process.
#
#   Eight scenarios cover drop, create, truncate, and verify against both transitions:
#     step_up   + drop/create/truncate/verify: schema op races follower->leader step-up
#     step_down + drop/create/truncate/verify: schema op races leader->follower step-down
#
#   Each scenario runs in a subprocess so that the expected abort is caught as a
#   non-zero exit code without killing the test runner.
#
#   FIXME-WT-17880: Remove this test once we have asynchronous step-up/step-down.

import signal, threading, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_stepup12(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_layered_stepup12'
    uri = 'layered:' + tablename
    new_uri = 'layered:' + tablename + '_new'
    num_rows = 100

    disagg_storages = gen_disagg_storages(disagg_only=True)
    transitions = [
        ('step_up',   dict(start_role='follower', target_role='leader')),
        ('step_down', dict(start_role='leader',   target_role='follower')),
    ]
    ops = [
        ('drop_lock_wait',   dict(op='drop', lock_wait=True)),
        ('drop_lock_nowait', dict(op='drop', lock_wait=False)),
        ('create',    dict(op='create')),
        ('truncate',  dict(op='truncate')),
        ('verify',    dict(op='verify')),
    ]
    scenarios = make_scenarios(disagg_storages, transitions, ops)

    conn_config = 'disaggregated=(role="leader",drain_threads=1)'

    def _race_scenario(self):
        # Create and populate the table as leader so the drop scenarios have something to drop.
        self.session.create(self.uri,
                            'key_format=i,value_format=S,block_manager=disagg,type=layered')
        c = self.session.open_cursor(self.uri)
        for i in range(self.num_rows):
            c[i] = 'value'
        c.close()
        self.close_conn()

        conn = wiredtiger.wiredtiger_open(
            self.home,
            'statistics=(all)'
            + self.extensionsConfig()
            + f',disaggregated=(role={self.start_role},drain_threads=2)')

        session = conn.open_session('')

        if self.start_role == 'follower':
            # Provide the leader checkpoint so the follower can open the table.
            meta = self.disagg_get_complete_checkpoint_meta(conn)
            self.assertIsNotNone(meta, 'expected a complete checkpoint from the leader')
            conn.reconfigure(f'disaggregated=(checkpoint_meta="{meta}")')
            session.open_cursor(self.uri).close()

        # Start the role transition in a background thread.
        t = threading.Thread(
            target=lambda: conn.reconfigure(f'disaggregated=(role={self.target_role})'),
            daemon=True)
        t.start()

        # The assertion in WT_WITH_SCHEMA_LOCK must fire and abort the process.
        match self.op:
            case 'drop':
                lw = 'true' if self.lock_wait else 'false'
                session.drop(self.uri, f'force=true,checkpoint_wait=false,lock_wait={lw}')
            case 'create':
                session.create(self.new_uri,
                                'key_format=i,value_format=S,block_manager=disagg,type=layered')
            case 'truncate':
                session.truncate(self.uri, None, None, None)
            case 'verify':
                session.verify(self.uri, None)

        t.join()

        conn.close()

    def subprocess_race(self):
        self._race_scenario()

    def test_race(self):
        rc, _ = self.run_subprocess_function(
            'SUBPROCESS',
            'test_layered_stepup12.test_layered_stepup12.subprocess_race',
            silent=True)
        self.assertEqual(rc, -signal.SIGABRT,
            f'expected process to abort (rc={-signal.SIGABRT}) but got rc={rc}')

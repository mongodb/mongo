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
#   Verify how schema operations on layered tables behave during a role transition, against both
#   step-up (follower->leader) and step-down (leader->follower):
#     - Schema ops that take only the schema lock (create, and drop with checkpoint_wait=false) can
#       race the transition, so they hit the "ongoing role-transition" guard and abort the process.
#     - Schema ops that take the checkpoint lock first (truncate, verify, and drop with
#       checkpoint_wait=true) are serialized against the transition by that lock - it is held for
#       the whole step up/down - so they never observe the transition and do not abort.
#     - Opening a statistics cursor acquires the schema lock to open a data handle, but a handle
#       open is not a schema operation, so it is allowed during a transition and must not abort.
#
#   Each scenario runs in a subprocess, so that the expected aborts are caught as a
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
        ('drop_lock_wait',
            dict(op='drop', checkpoint_wait=False, lock_wait=True,  expect_abort=True)),
        ('drop_lock_nowait',
            dict(op='drop', checkpoint_wait=False, lock_wait=False, expect_abort=True)),
        ('create',
            dict(op='create',                                       expect_abort=True)),
        ('drop_checkpoint_wait',
            dict(op='drop', checkpoint_wait=True,  lock_wait=True,  expect_abort=False)),
        ('truncate',
            dict(op='truncate',                                     expect_abort=False)),
        ('verify',
            dict(op='verify',                                       expect_abort=False)),
        ('stat_cursor',
            dict(op='stat_cursor',                                  expect_abort=False)),
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
            + ',timing_stress_for_test=[disagg_role_transition]'
            + f',disaggregated=(role={self.start_role},drain_threads=2)')

        session = conn.open_session('')

        if self.start_role == 'follower':
            # Provide the leader checkpoint so the follower can open the table.
            meta = self.disagg_get_complete_checkpoint_meta(conn)
            self.assertIsNotNone(meta, 'expected a complete checkpoint from the leader')
            conn.reconfigure(f'disaggregated=(checkpoint_meta="{meta}")')
            # Leave the handle unopened for the stat-cursor case so that opening the statistics
            # cursor during the transition triggers a first-time handle open.
            if self.op != 'stat_cursor':
                session.open_cursor(self.uri).close()

        # Start the role transition in a background thread.
        t = threading.Thread(
            target=lambda: conn.reconfigure(f'disaggregated=(role={self.target_role})'),
            daemon=True)

        transition_stat = wiredtiger.stat.conn.disagg_step_up_in_progress \
            if self.target_role == 'leader' else wiredtiger.stat.conn.disagg_step_down_in_progress

        # Wait until the role transition has begun before initiating the schema op.
        t.start()
        self.assertStatGreaterSoon(transition_stat, 0, session=session, timeout=10,
            msg='role transition did not start')

        match self.op:
            case 'drop':
                lw = 'true' if self.lock_wait else 'false'
                cw = 'true' if self.checkpoint_wait else 'false'
                session.drop(self.uri, f'force=true,checkpoint_wait={cw},lock_wait={lw}')
            case 'create':
                session.create(self.new_uri,
                                'key_format=i,value_format=S,block_manager=disagg,type=layered')
            case 'truncate':
                session.truncate(self.uri, None, None, None)
            case 'verify':
                session.verify(self.uri, None)
            case 'stat_cursor':
                session.open_cursor(
                    'statistics:' + self.uri, None, 'statistics=(all)').close()

        t.join()

        conn.close()

    def subprocess_race(self):
        self._race_scenario()

    def test_race(self):
        rc, _ = self.run_subprocess_function(
            'SUBPROCESS',
            'test_layered_stepup12.test_layered_stepup12.subprocess_race',
            silent=True,
            scenario=self.scenario_name)
        if self.expect_abort:
            self.assertEqual(rc, -signal.SIGABRT,
                f'expected process to abort (rc={-signal.SIGABRT}) but got rc={rc}')
        else:
            self.assertGreaterEqual(rc, 0,
                f'expected no abort but process was killed by signal (rc={rc})')

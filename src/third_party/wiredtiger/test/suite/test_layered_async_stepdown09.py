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

# test_layered_async_stepdown09.py
#   A planned step-down requires the step-down checkpoint to land on the step-down timestamp,
#   because that checkpoint is what the next leader picks up. A mismatch is a protocol violation
#   and aborts, so each case runs in a subprocess and is judged by its exit status.

import signal, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_async_stepdown09(LayeredStepdownMixin, wttest.WiredTigerTestCase,
                                    suite_subprocess):
    conn_config = 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    checkpoints = [
        ('at_cutoff',     dict(checkpoint_ts=20,   expect_abort=False)),
        ('before_cutoff', dict(checkpoint_ts=15,   expect_abort=True)),
        ('no_checkpoint', dict(checkpoint_ts=None, expect_abort=True)),
    ]
    scenarios = make_scenarios(disagg_storages, checkpoints)

    test_name = __qualname__

    uri = f'layered:{test_name}'
    cutoff = 20

    # Step down with stable at the cutoff, varying only where the last checkpoint sits.
    def _step_down_with_checkpoint_at(self, checkpoint_ts):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'v1'}, 10)

        self.set_step_down_ts(self.cutoff)
        if checkpoint_ts is not None:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(checkpoint_ts))
            ckpt_session = self.conn.open_session()
            ckpt_session.checkpoint()
            ckpt_session.close()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.cutoff))
        self.conn.reconfigure('disaggregated=(role="follower")')

    def subprocess_step_down(self):
        self._step_down_with_checkpoint_at(self.checkpoint_ts)

    def test_step_down_checkpoint_boundary(self):
        rc, _ = self.run_subprocess_function(
            'SUBPROCESS',
            'test_layered_async_stepdown09.test_layered_async_stepdown09.subprocess_step_down',
            silent=True,
            scenario=self.scenario_name)
        if self.expect_abort:
            self.assertEqual(rc, -signal.SIGABRT,
                f'expected the step down to abort (rc={-signal.SIGABRT}) but got rc={rc}')
        else:
            self.assertEqual(rc, 0, f'expected the step down to succeed but got rc={rc}')

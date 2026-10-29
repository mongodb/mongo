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

import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

# Every checkpoint crash point must have exactly one outcome. A crash taken before the checkpoint
# transaction commits always loses the checkpoint. A crash taken after it keeps the checkpoint
# whenever connection logging made the committed metadata durable, because recovery replays it.
class test_checkpoint_crash01(wttest.WiredTigerTestCase, suite_subprocess):
    test_name = __qualname__
    uri = f'table:{test_name}'

    # The numeric setting selects a tree, or at the top of its range the phase that follows them,
    # both of which precede the commit. Cover both ends: the top of the range is where a scaling
    # mistake lands on a point that is never reached.
    crash_points = [
        ('before_checkpoint_commit',
            dict(debug_config='checkpoint_crash_trigger_point=before_checkpoint_commit',
                 post_commit=False)),
        ('before_metadata_sync',
            dict(debug_config='checkpoint_crash_trigger_point=before_metadata_sync',
                 post_commit=True)),
        ('numeric_first', dict(debug_config='checkpoint_crash_point=1', post_commit=False)),
        ('numeric_last', dict(debug_config='checkpoint_crash_point=1000', post_commit=False)),
    ]
    logging = [
        ('logged', dict(logging=True)),
        ('not_logged', dict(logging=False)),
    ]
    scenarios = make_scenarios(crash_points, logging)

    def conn_config(self):
        return 'log=(enabled=%s)' % ('true' if self.logging else 'false')

    def subprocess_func(self):
        # Leave one key in a complete checkpoint and one written only into the crashed checkpoint,
        # so that the outcome is visible as whether rollback to stable keeps the second key.
        self.session.create(self.uri, 'key_format=S,value_format=S,log=(enabled=false)')
        c = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        c['k1'] = 'v1'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.session.begin_transaction()
        c['k2'] = 'v2'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Expected to kill this process.
        self.session.checkpoint('debug=(%s)' % self.debug_config)

    # FIXME-WT-16920: the disagg hook tracks layered uris per test case, so the parent cannot name a
    # table the subprocess created.
    @wttest.skip_for_hook("disagg", "the parent cannot name a table created in the subprocess")
    def test_checkpoint_crash(self):
        self.conn.close()

        home = self.crash_in_subprocess('SUBPROCESS',
            f'{self.test_name}.{self.test_name}.subprocess_func', self.debug_crash_signal)

        conn = wiredtiger.wiredtiger_open(home, self.conn_config())
        try:
            session = conn.open_session()
            c = session.open_cursor(self.uri)
            c.set_key('k1')
            self.assertEqual(c.search(), 0)
            c.set_key('k2')
            keeps_checkpoint = self.post_commit and self.logging
            self.assertEqual(c.search(), 0 if keeps_checkpoint else wiredtiger.WT_NOTFOUND)
        finally:
            conn.close()

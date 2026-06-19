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

import os
import wttest
from helper_disagg import disagg_test_class
from run import wt_builddir
from suite_subprocess import suite_subprocess

# Verify automatic pickup of the latest disaggregated checkpoint at open time:
#  - test_leader_auto_pickup exercises the in-library leader-mode pickup.
#  - test_follower_auto_pickup_via_wt exercises the util-driven follower
#    pickup performed by the wt tool when a checkpoint exists.
#  - test_follower_no_checkpoint_via_wt covers the same util path when the
#    page log has no completed checkpoint yet (pl_get_complete_checkpoint
#    returns WT_NOTFOUND); wt must still open cleanly with no metadata
#    installed.
#  - test_follower_picks_up_latest_checkpoint confirms that when multiple
#    complete checkpoints exist, the follower picks up the newest one
#    (overwritten values, not the earlier checkpoint's values).

@disagg_test_class
class test_disagg_util01(wttest.WiredTigerTestCase, suite_subprocess):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    create_session_config = 'key_format=i,value_format=S'
    nrows = 100

    def conn_config(self):
        return 'disaggregated=(role="leader")'

    def test_leader_auto_pickup(self):
        # Leader: create a table, write some rows, checkpoint.
        self.session.create(self.uri, self.create_session_config)
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = 'value' + str(i)
        cursor.close()
        self.session.checkpoint()

        # Step down so close does not write a shutdown checkpoint after ours.
        self.conn.reconfigure('disaggregated=(role="follower")')

        # Reopen as leader. Leader-mode pickup should pick up the latest checkpoint and
        # then begins the next checkpoint window so writes can resume.
        self.restart_without_local_files(
            config='disaggregated=(role="leader")',
            pickup_checkpoint=False)

        cursor = self.session.open_cursor(self.uri)
        seen = {k: v for k, v in cursor}
        cursor.close()
        self.assertEqual(len(seen), self.nrows,
            f"expected {self.nrows} rows after leader auto-pickup, got {len(seen)}")
        for i in range(self.nrows):
            self.assertEqual(seen[i], 'value' + str(i))

        # The new leader should be able to drive the next checkpoint
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows, self.nrows + 10):
            cursor[i] = 'value' + str(i)
        cursor.close()
        self.session.checkpoint()

    def _disagg_extension_path(self):
        ext_dir = os.path.join(wt_builddir, 'ext', 'page_log', self.ds_name)
        candidates = [os.path.join(ext_dir, e) for e in os.listdir(ext_dir)
                      if e.endswith('.so') or e.endswith('.dylib')]
        self.assertEqual(len(candidates), 1,
            f"expected exactly one page-log shared object under {ext_dir}, got {candidates}")
        return candidates[0]

    def _run_wt_as_follower(self, name, wt_args):
        # Step down and close so the wt utility can attach to the page log as a fresh
        # follower; spawn `wt <wt_args>` against a sibling home that shares kv_home
        # with the leader. Returns the captured (stdout, stderr) text.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.close_conn()

        follower_home = os.path.join(self.home, name)
        os.mkdir(follower_home)
        os.symlink('../kv_home', os.path.join(follower_home, 'kv_home'),
            target_is_directory=True)

        ext_path = self._disagg_extension_path()
        page_log = self.page_log()
        config = (f'create,'
                  f'extensions=[{ext_path}=(config="(verbose=0)")],'
                  f'disaggregated=(role="follower",page_log={page_log})')
        outfile = f'{name}.out'
        errfile = f'{name}.err'
        self.runWt(['-h', follower_home, '-C', config] + list(wt_args),
                   outfilename=outfile, errfilename=errfile, closeconn=False)
        with open(outfile) as f:
            out = f.read()
        with open(errfile) as f:
            err = f.read()
        return out, err

    def test_follower_auto_pickup_via_wt(self):
        # Leader: create a table, write some rows, checkpoint.
        self.session.create(self.uri, self.create_session_config)
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = 'value' + str(i)
        cursor.close()
        self.session.checkpoint()

        out, _ = self._run_wt_as_follower('wt-follower', ['list'])
        self.assertIn(f'layered:{self.test_name}', out,
            f"expected 'layered:{self.test_name}' in wt list output, got:\n{out}")

    def test_follower_no_checkpoint_via_wt(self):
        # Don't write or checkpoint anything as leader; wt must still open cleanly
        # and surface the "no checkpoint" notice on stderr.
        _, err = self._run_wt_as_follower('wt-follower-empty', ['list'])
        self.assertIn('no complete checkpoint found', err,
            f"expected 'no complete checkpoint found' in wt stderr, got:\n{err}")

    def test_follower_picks_up_latest_checkpoint(self):
        # Leader: write initial values and checkpoint.
        self.session.create(self.uri, self.create_session_config)
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = 'old' + str(i)
        cursor.close()
        self.session.checkpoint()

        # Overwrite values and checkpoint again; the follower must see these.
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = 'new' + str(i)
        cursor.close()
        self.session.checkpoint()

        out, _ = self._run_wt_as_follower('wt-follower-latest', ['dump', self.uri])
        self.assertIn('new0', out,
            f"expected newer values in wt dump output, got:\n{out}")
        self.assertNotIn('old0', out,
            f"unexpected older values in wt dump output:\n{out}")

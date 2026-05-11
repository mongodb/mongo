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

import wttest
from helper_disagg import disagg_test_class

# test_layered102.py
# Verify that startup database-size verification waits until checkpoint pickup.

@disagg_test_class
class test_layered102(wttest.WiredTigerTestCase):
    uri = 'layered:test_layered102'
    create_session_config = 'key_format=i,value_format=S'

    def conn_config(self):
        return f'verbose=[verify:3],disaggregated=(role="leader")'

    def _follower_config(self, checkpoint_meta=None):
        config = f'verbose=[verify:3],verify_metadata=true,' \
            f'disaggregated=(role="follower"'
        if checkpoint_meta is not None:
            config += f',checkpoint_meta="{checkpoint_meta}"'
        config += ')'
        return config

    def test_startup_verify_db_size(self):
        self.session.create(self.uri, self.create_session_config)

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value' + str(i)
        cursor.close()

        self.session.checkpoint()
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        # Reopen as a follower without checkpoint metadata first. The startup
        # verify path should skip the database-size check until pickup occurs.
        # Step down before close so close does not create a shutdown checkpoint.
        # That keeps this reopen on the no-pickup path until checkpoint_meta is supplied.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.close_conn()
        with self.customStdoutPattern(
            lambda output: self.assertNotRegex(
                output, r'disagg database size verify: .*checkpoint size')):
            self.open_conn(config=self._follower_config())

        # Reopen again with checkpoint metadata so startup picks up the latest
        # checkpoint and runs the database-size verification branch.
        with self.expectedStdoutPattern(
            r'disagg database size verify: .*checkpoint size', maxchars=30000):
            self.reopen_conn(config=self._follower_config(checkpoint_meta))

        # test_layered*.py modules run layered verify during teardown. Ignore the
        # resulting verify verbosity so teardown only checks this test's output.
        self.ignoreStdoutPattern(r'WT_SESSION\.verify: \[WT_VERB_VERIFY\]')

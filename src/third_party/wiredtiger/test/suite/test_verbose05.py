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
#

from test_verbose01 import test_verbose_base
import wttest
from helper import WiredTigerCursor
import re, time

# Verify checkpoint progress verbose logging gates intermediate progress messages on a time
# interval: a checkpoint that completes within WT_PROGRESS_MSG_PERIOD logs none, and a
# checkpoint forced to run longer logs at least one.
@wttest.skip_for_hook("disagg", "Checkpoint progress output is different under disagg")
class test_verbose05(test_verbose_base):

    test_name = __qualname__
    uri = f'table:{test_name}'
    create_config = 'key_format=S,value_format=S,allocation_size=4KB,leaf_page_max=4KB,memory_page_max=4KB'
    conn_config = 'statistics=(all),verbose=[checkpoint_progress:0]'

    # Force a checkpoint to run past WT_PROGRESS_MSG_PERIOD: the stressors add a 10 second
    # delay up front, then a 2 second delay per table, so with 20 tables the checkpoint is
    # still writing pages past two message periods.
    slow_checkpoint_config = 'timing_stress_for_test=[checkpoint_slow,checkpoint_handle]'
    slow_checkpoint_tables = 20

    # Mirror of WT_PROGRESS_MSG_PERIOD: progress messages are gated on whole multiples of it.
    progress_msg_period = 20

    progress_pattern = re.compile(
        r'WT_VERB_CHECKPOINT_PROGRESS.*Checkpoint has been running for \d+ seconds, wrote \d+' \
        r' pages \(\d+ MB\), walked \d+ pages and checkpointed \d+ files')

    def populate(self, session, uri, row_count, seed):
        with WiredTigerCursor(session, uri) as cursor:
            for key in range(row_count):
                # Use long string to increase the pages
                cursor[str(key)] = seed*4000

    def checkpoint_and_count_progress(self, table_count):
        session = self.session
        for i in range(table_count):
            uri = self.uri if i == 0 else '{}_{}'.format(self.uri, i)
            session.create(uri, self.create_config)
            self.populate(session, uri, 50, 'x')
        start = time.monotonic()
        session.checkpoint()
        elapsed = time.monotonic() - start
        return len(self.progress_pattern.findall(self.readStdout(100000))), elapsed

    def finish_and_clean_output(self):
        # Checkpoint prepare always logs a final progress message, and closing the connection
        # runs its own checkpoint; ignore that expected output, which isn't what this test checks.
        self.ignoreStdoutPattern(
            r'WT_VERB_CHECKPOINT_PROGRESS.*(Checkpoint (prepare )?ran|saving checkpoint snapshot)')
        self.cleanStdout()
        self.conn.reconfigure('verbose=[]')

    def test_checkpoint_progress_fast_checkpoint(self):
        log_count, _ = self.checkpoint_and_count_progress(1)
        self.assertEqual(log_count, 0,
            "Intermediate progress messages emitted for a checkpoint shorter than the progress " \
            "message period: {}".format(log_count))
        self.finish_and_clean_output()

    def test_checkpoint_progress_slow_checkpoint(self):
        self.conn.reconfigure(self.slow_checkpoint_config)
        log_count, elapsed = self.checkpoint_and_count_progress(self.slow_checkpoint_tables)
        # Clear the timing stress so the checkpoint at connection close isn't delayed.
        self.conn.reconfigure('timing_stress_for_test=[]')
        # The final checkpoint prepare message always logs and uses up the first message period,
        # so the first intermediate progress message can't appear until two periods have passed.
        self.assertGreaterEqual(elapsed, 2 * self.progress_msg_period,
            "Timing stress didn't stretch the checkpoint past two progress message periods: "
            "{:.1f} seconds".format(elapsed))
        self.assertGreaterEqual(log_count, 1,
            "No intermediate progress messages emitted for a checkpoint running past the "
            "progress message period")
        self.finish_and_clean_output()

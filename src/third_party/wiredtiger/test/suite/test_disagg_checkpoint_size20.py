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

import wttest
from helper_disagg import disagg_test_class

# test_disagg_checkpoint_size20.py
#   Regression test for the root-size accounting drift across connection
#   restarts on disagg.
#
# Background:
#   At connection open, the disagg block manager initialises its running
#   byte total for a stable file from the checkpoint size recorded in the
#   file's metadata, and separately reads the file's root address to remember
#   the current root page's size. These two are derived independently from
#   the on-disk metadata.
#
#   For an empty/initial checkpoint, the recorded checkpoint size can be 0
#   while the root address still publishes a valid cookie carrying a non-zero
#   root size. Before the fix the running total did not account for the
#   root bytes that the cookie advertised, so the next checkpoint's
#   root-size transition subtracted the previous root size from a running
#   total that never included it. The subsequent chain discard then
#   underflowed the running total.
#
# Fix:
#   The checkpoint-load path now seeds the running total from the root cookie's
#   size so the invariant that the running total is at least the current root size
#   holds before any root-size transition runs. The (re-)initialisation
#   path also resets the previous/current root-size bookkeeping when the
#   running total is re-initialised from metadata, clearing stale state that
#   the block manager's per-file handle cache may have left behind.
#
# Scenario reproduced by this test:
#   1. Open as leader, create a layered table, insert data, checkpoint.
#      The shutdown checkpoint on close publishes a non-empty root address
#      for the shared metadata file.
#   2. Reopen the connection: the running total and current root size are
#      re-derived from the file's metadata. They must agree.
#   3. Write more rows; the next checkpoint exercises the root-size
#      transition on the metadata file. A subsequent chain discard hits
#      the running-total decrement; an off-by-root-size drift here would
#      trip the assertion guarding against underflow.
#
#   The drift is deterministic: the stale root-size accumulates across restarts
#   and causes an underflow on the third restart. Four cycles catches the crash
#   and verifies the running total stays clean for one cycle beyond it.

@disagg_test_class
class test_disagg_checkpoint_size20(wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size20'
    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true)'
    uri = 'layered:' + uri_base
    table_config = 'key_format=S,value_format=S'

    nrows = 200
    cycles = 4

    def insert_rows(self, start, count, value_char):
        c = self.session.open_cursor(self.uri)
        for i in range(start, start + count):
            c[f'key{i:08d}'] = value_char * 100
        c.close()

    def test_root_size_consistent_across_restart(self):
        # The reopen warns about removing local disagg files; that is the
        # expected initialisation path, not an error.
        self.session.create(self.uri, self.table_config)

        # Cycle: write, checkpoint, restart. Each restart exercises the
        # running-total re-initialisation and checkpoint-load sequence on
        # the shared metadata file.
        for cycle in range(self.cycles):
            char = chr(ord('A') + (cycle % 26))
            start = cycle * self.nrows
            self.insert_rows(start, self.nrows, char)
            self.session.checkpoint()

            with self.expectedStdoutPattern('Removing local file'):
                self.reopen_conn()

            # Force a checkpoint on the fresh connection so the metadata
            # file's root-size transition runs against the just-initialised
            # running total and current root size. Insert a row first so
            # the checkpoint has work to do and the chain is non-empty.
            self.insert_rows(start + self.nrows // 2, 1, char)
            self.session.checkpoint()

        # Sanity: data is still readable after all the restarts.
        c = self.session.open_cursor(self.uri)
        count = sum(1 for _ in c)
        c.close()
        self.assertGreater(count, 0,
            'No rows readable after restart cycles')

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

import json, os, subprocess
import wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, DisaggCorruptionMixin, get_shard_id
from metadata_helper import get_table_id
from run import wt_builddir
from suite_subprocess import suite_subprocess

# Reading individual pages in follower mode without a checkpoint pickup.
# The tool must start when the checkpoint is corrupt, and
# `wt page -t` must read intact data pages directly off the page log.
@wttest.skip_for_hook("tiered", "wt page does not run under tiered hook")
class test_disagg_util03(wttest.WiredTigerTestCase, suite_subprocess,
                         DisaggConfigMixin, DisaggCorruptionMixin):
    uri = "layered:util03"
    stable_uri = "file:util03.wt_stable"
    nrows = 1000
    conn_config = 'disaggregated=(role="leader")'

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    def _follower_config(self):
        return self.extensionsConfig() + ',disaggregated=(role="follower")'

    def _run_wt(self, *args, failure=False):
        cmd = ['-C', self._follower_config()] + list(args)
        # Do not reopen the session after wt exits; the test does not need it
        # again and the corrupt checkpoint would make open_conn() fail anyway.
        self.runWt(cmd, outfilename='wt.out', errfilename='wt.err',
                   failure=failure, reopensession=False)
        with open('wt.out') as f:
            out = f.read()
        with open('wt.err') as f:
            err = f.read()
        return out, err

    def _populate(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            c[f"k{i:08}"] = f"v{i:08}"
        c.close()
        self.session.checkpoint()

    def test_tool_starts_with_corrupt_checkpoint(self):
        if self.ds_name != 'palite':
            self.skipTest('palite-only test')
        self._populate()
        self.corrupt_checkpoint_metadata_page()
        # `wt list` needs no page data; with empty metadata it lists nothing
        # and must exit zero, with a warning rather than an abort.
        _, err = self._run_wt('list')
        self.assertIn('proceeding with empty metadata', err)

    def _find_base_image_page(self):
        # Largest base-image row (no backlink) for the data table: the leaf
        # data page rather than the small internal root.
        table_id = get_table_id(self.session, self.stable_uri)
        shard = get_shard_id(table_id)
        db = os.path.join(self.home, 'kv_home', f'pages_{shard:02d}.db')
        sql = (f"SELECT page_id, lsn FROM pages WHERE table_id={table_id} "
               f"AND base_lsn=0 AND backlink_lsn=0 "
               f"ORDER BY length(page_data) DESC LIMIT 1;")
        sqlite_exe = os.path.join(wt_builddir, 'sqlite3')
        out = subprocess.run([sqlite_exe, '-json', db, sql],
                             capture_output=True, text=True, check=True).stdout
        rows = json.loads(out) if out.strip() else []
        self.assertTrue(rows, f"no base-image row for table_id={table_id}")
        return table_id, int(rows[0]['page_id']), int(rows[0]['lsn'])

    def test_raw_read_after_corrupt_checkpoint(self):
        if self.ds_name != 'palite':
            self.skipTest('palite-only test')
        if not wiredtiger.diagnostic_build():
            self.skipTest('wt page requires a diagnostic build')
        self._populate()
        table_id, page_id, lsn = self._find_base_image_page()
        self.corrupt_checkpoint_metadata_page()
        out, err = self._run_wt(
            'page', '-t', str(table_id), '-p', str(page_id), '-l', str(lsn))
        self.assertIn('proceeding with empty metadata', err)
        # The page-log metadata is printed to stdout; the page image itself
        # is dumped as raw bytes to stderr.
        self.assertIn(f"table_id: {table_id}", out)
        self.assertIn("results: count=1", out)
        self.assertIn(f"base of 0 delta(s): page_id {page_id}", err)
        self.assertIn("(chunk 2 of ", err)

    def test_raw_read_unknown_page(self):
        if self.ds_name != 'palite':
            self.skipTest('palite-only test')
        if not wiredtiger.diagnostic_build():
            self.skipTest('wt page requires a diagnostic build')
        self._populate()
        table_id = get_table_id(self.session, self.stable_uri)
        _, err = self._run_wt(
            'page', '-t', str(table_id), '-p', '99999999', '-l', '1',
            failure=True)
        self.assertIn("WT_NOTFOUND", err)

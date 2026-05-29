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

import json, os, re, subprocess
from typing import NamedTuple
import wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, get_shard_id
from metadata_helper import get_table_id
from run import wt_builddir
from suite_subprocess import suite_subprocess

class PalitePage(NamedTuple):
    """One row from the palite pages table. Schema in ext/page_log/palite/palite.cpp."""
    page_id: int
    lsn: int
    base_lsn: int
    backlink_lsn: int
    flags: int

# Test the `wt page` command against a palite backed disaggregated storage database.
# A leader connection writes full-image and delta pages via checkpoints, then `wt page`
# is run as a subprocess in follower mode against the same cell to inspect them.
@wttest.skip_for_hook("tiered", "wt page does not run under tiered hook")
class test_disagg_wt_page(wttest.WiredTigerTestCase, suite_subprocess, DisaggConfigMixin):
    uri = "layered:wt_page_test"
    stable_uri = "file:wt_page_test.wt_stable"
    nrows = 1000

    # palite flag bit indicating a tombstoned page chain entry; mirrors
    # WT_PAGE_LOG_DISCARDED in ext/page_log/palite/palite.cpp.
    PAGE_LOG_DISCARDED = 0x10000

    conn_config = 'disaggregated=(role="leader")'

    # Skip inside the test method (not setUp): unittest does not call tearDown
    # when skipTest is raised from setUp, which would leak the open connection.
    def _skip_if_not_diagnostic(self):
        if not wiredtiger.diagnostic_build():
            self.skipTest('wt page requires a diagnostic build')

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    def _wt_page_extra_config(self):
        return self.extensionsConfig() + ',disaggregated=(role="follower")'

    # Returns (stdout, stderr); failure=True asserts a non-zero exit.
    def _run_wt_page(self, *args, failure=False):
        cmd = ['-C', self._wt_page_extra_config(), 'page'] + list(args)
        self.runWt(cmd, outfilename='wt.out', errfilename='wt.err',
                   failure=failure)
        with open('wt.out') as f:
            stdout = f.read()
        with open('wt.err') as f:
            stderr = f.read()
        return stdout, stderr

    def _populate(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            c[f"k{i:08}"] = f"v{i:08}"
        c.close()
        self.session.checkpoint()

    def _dirty_and_checkpoint(self):
        # Update a scattered subset of keys so palite emits delta entries.
        c = self.session.open_cursor(self.uri)
        for i in range(0, self.nrows, max(1, self.nrows // 8)):
            c[f"k{i:08}"] = f"V{i:08}"
        c.close()
        self.session.checkpoint()

    # Find the newest page chain entry matching where_clause. Shells out to
    # the sqlite3 binary built alongside palite; the system Python sqlite3
    # may be too old to parse the palite schema.
    def _find_page(self, where_clause, description):
        table_id = get_table_id(self.session, self.stable_uri)
        db = os.path.join(self.home, 'kv_home',
                          f'pages_{get_shard_id(table_id):02d}.db')
        sql = (f"SELECT page_id, lsn, base_lsn, backlink_lsn, flags "
               f"FROM pages WHERE table_id={table_id} AND {where_clause} "
               f"ORDER BY lsn DESC LIMIT 1;")
        sqlite_exe = os.path.join(wt_builddir, 'sqlite3')
        out = subprocess.run([sqlite_exe, '-json', db, sql],
                             capture_output=True, text=True, check=True).stdout
        rows = json.loads(out) if out.strip() else []
        self.assertTrue(rows,
            f"no {description} rows for table_id={table_id} in palite")
        r = rows[0]
        return PalitePage(r['page_id'], r['lsn'], r['base_lsn'],
                          r['backlink_lsn'], r['flags'])

    def _find_base_image_page(self):
        return self._find_page("base_lsn=0 AND backlink_lsn=0", "base-image")

    def _find_delta_page(self):
        return self._find_page(
            f"backlink_lsn != 0 AND (flags & {self.PAGE_LOG_DISCARDED}) = 0",
            "delta")

    def _assert_chain_header(self, stdout, page):
        self.assertIn(
            f"disagg_meta: page_id={page.page_id} lsn={page.lsn} "
            f"base_lsn={page.base_lsn} backlink_lsn={page.backlink_lsn} ",
            stdout)
        return int(re.search(r"^results: count=(\d+)$", stdout, re.M).group(1))

    def test_help(self):
        self._skip_if_not_diagnostic()
        _, stderr = self._run_wt_page('-?')
        self.assertIn('page -p page_id', stderr)
        self.assertIn('-l lsn', stderr)

    def test_unknown_page_id(self):
        self._skip_if_not_diagnostic()
        self._populate()
        _, stderr = self._run_wt_page("-p", "99999999", "-l", "1",
                                      self.stable_uri, failure=True)
        self.assertIn("WT_NOTFOUND", stderr)

    def test_missing_required_l(self):
        self._skip_if_not_diagnostic()
        self._populate()
        _, stderr = self._run_wt_page("-p", "1", self.stable_uri, failure=True)
        self.assertIn("-l lsn is required", stderr)

    def test_full_image(self):
        self._skip_if_not_diagnostic()
        self._populate()
        page = self._find_base_image_page()
        stdout, _ = self._run_wt_page(
            "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri)
        self.assertEqual(self._assert_chain_header(stdout, page), 1)
        self.assertIn("- row-store ", stdout)

    def test_delta_chain(self):
        self._skip_if_not_diagnostic()
        self._populate()
        self._dirty_and_checkpoint()
        page = self._find_delta_page()
        stdout, _ = self._run_wt_page(
            "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri)
        self.assertGreater(self._assert_chain_header(stdout, page), 1)

    def test_missing_required_p(self):
        self._skip_if_not_diagnostic()
        self._populate()
        _, stderr = self._run_wt_page(self.stable_uri, failure=True)
        self.assertIn("-p page_id is required", stderr)

if __name__ == '__main__':
    wttest.run()

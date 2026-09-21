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
from helper_disagg import DisaggConfigMixin, DisaggCorruptionMixin, get_shard_id
from helper_wt_corruption import parse_verify_leaves
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
class test_disagg_wt_page(
        wttest.WiredTigerTestCase, suite_subprocess, DisaggConfigMixin, DisaggCorruptionMixin):
    uri = "layered:wt_page_test"
    stable_uri = "file:wt_page_test.wt_stable"
    nrows = 1000

    # palite stores the put-args flags; these mirror the WT_PAGE_LOG_* bits in
    # ext/page_log/palite/palite.cpp.
    PAGE_LOG_COMPRESSED = 0x1
    PAGE_LOG_DELTA = 0x2
    PAGE_LOG_DISCARDED = 0x10000

    # delta_pct=100 lifts the delta-size threshold so a delta is always
    # preferred over a rewrite.
    conn_config = 'disaggregated=(role="leader"),page_delta=(delta_pct=100)'

    # Skip inside the test method (not setUp): unittest does not call tearDown
    # when skipTest is raised from setUp, which would leak the open connection.
    def _skip_if_not_diagnostic(self):
        if not wiredtiger.diagnostic_build():
            self.skipTest('wt page requires a diagnostic build')

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('compressors', 'zstd')
        DisaggConfigMixin.conn_extensions(self, extlist)

    def _wt_page_extra_config(self):
        # A follower cannot open a live stable table: the standalone tool opens the
        # (copied, offline) directory as the leader to inspect it.
        return self.extensionsConfig() + ',disaggregated=(role="leader")'

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
        self.ts_count = getattr(self, 'ts_count', 0) + 1
        self.session.begin_transaction()
        for i in range(self.nrows):
            c[f"k{i:08}"] = f"v{i:08}"
        c["secret_key"] = "s3cr3t_v4lue"
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.ts_count))
        c.close()
        self.session.checkpoint()

    def _dirty_and_checkpoint(self):
        # Update a scattered subset of keys so palite emits delta entries.
        c = self.session.open_cursor(self.uri)
        self.ts_count = getattr(self, 'ts_count', 0) + 1
        self.session.begin_transaction()
        for i in range(0, self.nrows, max(1, self.nrows // 8)):
            c[f"k{i:08}"] = f"V{i:08}"
        c["secret_key"] = "s3cr3t_v4lue_v2"
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.ts_count))
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
        # base_lsn=0 AND backlink_lsn=0 also matches the root (a full image
        # with no backlink), so pick the leaf by decoding the tree with `wt
        # verify -d dump_address` rather than guessing from palite's schema,
        # which has no page-type column to distinguish leaf from root.
        cmd = ['-C', self._wt_page_extra_config(), 'verify', '-d', 'dump_address',
               self.stable_uri]
        self.runWt(cmd, outfilename='wt.out', errfilename='wt.err')
        with open('wt.out') as f:
            stdout = f.read()
        leaves = parse_verify_leaves(stdout, disagg=True)
        self.assertEqual(len(leaves), 1, f"expected a single leaf, got {leaves}")
        page_id, _ = leaves[0]
        return self._find_page(
            f"page_id={page_id} AND base_lsn=0 AND backlink_lsn=0", "base-image")

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
        self.assertIn('-p page_id', stderr)
        self.assertIn('-l lsn', stderr)
        self.assertIn('unredact all application data', stderr)
        self.assertIn('display only the keys in the application data', stderr)

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
        self.assertNotIn("secret_key", stdout)
        self.assertNotIn("s3cr3t_v4lue", stdout)
        self.assertIn("{REDACTED}", stdout)

        stdout, _ = self._run_wt_page(
            "-u", "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri)
        self.assertEqual(self._assert_chain_header(stdout, page), 1)
        self.assertIn("secret_key", stdout)
        self.assertIn("s3cr3t_v4lue", stdout)
        self.assertNotIn("{REDACTED}", stdout)

    def test_full_image_corrupt(self):
        self._skip_if_not_diagnostic()
        self._populate()
        page = self._find_base_image_page()
        table_id = get_table_id(self.session, self.stable_uri)
        self.corrupt_page_image_at(table_id, page.page_id, page.lsn)

        _, stderr = self._run_wt_page(
            "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri, failure=True)
        self.assertIn(f"page_id {page.page_id}, lsn {page.lsn}", stderr)
        self.assertIn("{REDACTED}", stderr)

        _, stderr = self._run_wt_page(
            "-u", "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri, failure=True)
        self.assertIn(f"page_id {page.page_id}, lsn {page.lsn}", stderr)
        self.assertNotIn("{REDACTED}", stderr)

    def test_delta_chain(self):
        self._skip_if_not_diagnostic()
        self._populate()
        self._dirty_and_checkpoint()
        page = self._find_delta_page()

        stdout, _ = self._run_wt_page(
            "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri)
        result_count = self._assert_chain_header(stdout, page)
        self.assertGreater(result_count, 1)
        self.assertEqual(stdout.count("- delta page"), result_count - 1)
        self.assertIn("delta_op: update", stdout)
        self.assertNotIn("s3cr3t_v4lue_v2", stdout)
        self.assertIn("{REDACTED}", stdout)
        # The tagged value line proves the delta path's own unredact gate is
        # exercised, not just the base image's (the line above passes
        # regardless of the delta gate).
        self.assertIn("V: {REDACTED}", stdout)

        stdout, _ = self._run_wt_page(
            "-u", "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri)
        result_count = self._assert_chain_header(stdout, page)
        self.assertGreater(result_count, 1)
        self.assertIn("delta_op: update", stdout)
        self.assertIn("s3cr3t_v4lue_v2", stdout)
        self.assertNotIn("{REDACTED}", stdout)

    def test_full_image_keys_only(self):
        self._skip_if_not_diagnostic()
        self._populate()
        page = self._find_base_image_page()
        stdout, _ = self._run_wt_page(
            "-k", "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri)
        self.assertEqual(self._assert_chain_header(stdout, page), 1)
        self.assertIn("secret_key", stdout)
        self.assertNotIn("s3cr3t_v4lue", stdout)
        self.assertIn("{REDACTED}", stdout)

    def test_delta_chain_keys_only(self):
        self._skip_if_not_diagnostic()
        self._populate()
        self._dirty_and_checkpoint()
        page = self._find_delta_page()
        stdout, _ = self._run_wt_page(
            "-k", "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri)
        result_count = self._assert_chain_header(stdout, page)
        self.assertGreater(result_count, 1)
        self.assertIn("delta_op: update", stdout)
        # Scope to the delta section: secret_key is also in the base image, which is
        # always unredacted under -k regardless of the delta path's own gating.
        delta_section = stdout.split("- delta page", 1)[1]
        self.assertIn("secret_key", delta_section)
        self.assertNotIn("s3cr3t_v4lue_v2", delta_section)
        # Value stays redacted even though the key is shown.
        self.assertIn("V: {REDACTED}", delta_section)

    def test_conflicting_redact_flags(self):
        self._skip_if_not_diagnostic()
        self._populate()
        page = self._find_base_image_page()
        _, stderr = self._run_wt_page(
            "-u", "-k", "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri,
            failure=True)
        self.assertIn("mutually exclusive", stderr)

    def test_delta_chain_with_deletes(self):
        self._skip_if_not_diagnostic()
        self._populate()
        c = self.session.open_cursor(self.uri)
        self.ts_count = getattr(self, 'ts_count', 0) + 1
        delete_ts = self.ts_count
        self.session.begin_transaction()
        ndeletes = 0
        for i in range(0, self.nrows, max(1, self.nrows // 8)):
            c.set_key(f"k{i:08}")
            self.assertEqual(c.remove(), 0)
            ndeletes += 1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(delete_ts))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(delete_ts))
        c.close()
        self.session.checkpoint()
        page = self._find_delta_page()
        stdout, _ = self._run_wt_page(
            "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri)
        self.assertGreater(self._assert_chain_header(stdout, page), 1)
        # A commit-timestamped delete cannot be packed into a leaf page delta
        # (the delta delete op requires a globally-visible, untimestamped
        # tombstone); each delete instead appears as a delta update cell whose
        # stop window carries the timestamp of the delete.
        self.assertEqual(stdout.count(
            f'stop: durable_timestamp=(0, {delete_ts}) timestamp=(0, {delete_ts})'), ndeletes)

    def test_delta_chain_compressed(self):
        self._skip_if_not_diagnostic()
        # One large leaf so the delta below stays a small fraction of the page
        # (reconciliation keeps it as a delta instead of rewriting), while still
        # being larger than the allocation unit so zstd shrinks it and the block
        # is stored compressed.
        self.session.create(self.uri,
            "key_format=S,value_format=S,block_compressor=zstd,"
            "leaf_page_max=10MB,memory_page_max=10MB")
        c = self.session.open_cursor(self.uri)
        self.ts_count = getattr(self, 'ts_count', 0) + 1
        self.session.begin_transaction()
        for i in range(self.nrows):
            c[f"k{i:08}"] = "v" * 100
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.ts_count))
        c.close()
        self.session.checkpoint()
        c = self.session.open_cursor(self.uri)
        self.ts_count = getattr(self, 'ts_count', 0) + 1
        self.session.begin_transaction()
        for i in range(100):
            c[f"k{i:08}"] = "V" * 100
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.ts_count))
        c.close()
        self.session.checkpoint()

        # Require both compressed and delta bits so we skip compressed
        # full-image rewrites (which also carry a backlink).
        page = self._find_page(
            f"(flags & {self.PAGE_LOG_COMPRESSED}) != 0 AND "
            f"(flags & {self.PAGE_LOG_DELTA}) != 0 AND "
            f"(flags & {self.PAGE_LOG_DISCARDED}) = 0",
            "compressed delta")
        stdout, _ = self._run_wt_page(
            "-p", str(page.page_id), "-l", str(page.lsn), self.stable_uri)
        self.assertGreater(self._assert_chain_header(stdout, page), 1)
        self.assertIn("delta_op: update", stdout)

    def test_missing_required_p(self):
        self._skip_if_not_diagnostic()
        self._populate()
        _, stderr = self._run_wt_page(self.stable_uri, failure=True)
        self.assertIn("-p page_id is required", stderr)

if __name__ == '__main__':
    wttest.run()

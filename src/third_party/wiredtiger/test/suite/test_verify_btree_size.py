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

import os
import re
import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_verify_btree_size -- the fix_btree_size verify config corrects a drifted
# checkpoint metadata size on disaggregated btrees.
@disagg_test_class
class test_verify_btree_size(wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader")'

    uri_base = test_name
    uri = "layered:" + uri_base
    stable_uri = "file:" + uri_base + ".wt_stable"

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    nentries = 5000

    # Printed to stdout when verify with fix_btree_size corrects a size.
    correcting_pattern = r'WT_VERB_VERIFY.*size mismatch detected.*correcting'

    def populate(self, uri=None):
        cursor = self.session.open_cursor(uri or self.uri, None)
        for i in range(self.nentries):
            cursor[str(i)] = str(i) * 100
        cursor.close()
        self.session.checkpoint()

    def get_ckpt_size(self, stable_uri=None):
        # Return the size= value from the stable file's most recent checkpoint metadata.
        stable_uri = stable_uri or self.stable_uri
        cursor = self.session.open_cursor('metadata:', None)
        cursor.set_key(stable_uri)
        self.assertEqual(cursor.search(), 0)
        value = cursor.get_value()
        cursor.close()
        sizes = re.findall(r',size=(\d+),', value)
        self.assertGreater(len(sizes), 0, f"no checkpoint size in metadata for {stable_uri}")
        return int(sizes[0])

    def set_ckpt_size(self, new_size, stable_uri=None):
        # Overwrite the size= value in the stable file's checkpoint metadata.
        stable_uri = stable_uri or self.stable_uri
        cursor = self.session.open_cursor('metadata:', None, 'readonly=0')
        cursor.set_key(stable_uri)
        self.assertEqual(cursor.search(), 0)
        value = cursor.get_value()

        new_value = re.sub(r',size=\d+,', f',size={new_size},', value, count=1)
        self.assertNotEqual(new_value, value, "metadata had no size= field to replace")

        cursor.set_value(new_value)
        cursor.update()
        cursor.close()

    def test_fix_btree_size(self):
        # Corrupt a checkpoint size, then confirm verify with fix_btree_size corrects it and
        # the corrected size is visible via the repair API.
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.populate()

        real_size = self.get_ckpt_size()
        self.assertGreater(real_size, 0)

        # Corrupt the checkpoint size in the metadata.
        self.set_ckpt_size(1)

        # Verify with fix_btree_size off must not correct the metadata.
        # Both diagnostic and non-diagnostic builds print the size mismatch warning to stdout;
        # diagnostic builds additionally return WT_ERROR.
        pattern = r'WT_VERB_VERIFY.*checkpoint size .* does not match'
        with self.customStdoutPattern(lambda output: self.assertRegex(output, pattern)):
            if wiredtiger.diagnostic_build():
                self.assertRaisesException(wiredtiger.WiredTigerError,
                    lambda: self.session.verify(self.uri, 'strict=true,fix_btree_size=false'),
                    '/WT_ERROR/')
                self.ignoreStderrPatternIfExists('stable table verification failed')
            else:
                self.verifyUntilSuccess(
                    self.session, self.uri, config='strict=true,fix_btree_size=false')
        self.assertEqual(self.get_ckpt_size(), 1,
            "verify without fix_btree_size must not correct the metadata")

        # Verify with fix_btree_size on corrects the metadata size.
        with self.customStdoutPattern(
                lambda output: self.assertRegex(output, self.correcting_pattern)):
            self.verifyUntilSuccess(self.session, self.uri, config='strict=true,fix_btree_size=true')
        corrected_size = self.get_ckpt_size()
        self.assertEqual(corrected_size, real_size,
            "fix_btree_size should restore the size derived from the tree, which should match the "
            "original checkpoint size")

        # The corrected size is visible via the repair API.
        meta = wiredtiger.wiredtiger_repair(
            self.conn, f'fetch_metadata=(local=true,uri="{self.stable_uri}")')
        sizes = re.findall(r',size=(\d+),', meta)
        self.assertGreater(len(sizes), 0, f"no size in fetch_metadata output: {meta}")
        # The first size= value is the checkpoint size, consistent with get_ckpt_size.
        self.assertEqual(int(sizes[0]), corrected_size,
            f"fetch_metadata size {sizes[0]} != corrected size {corrected_size}")

    def test_fix_btree_size_via_repair(self):
        # The wiredtiger_repair fix_btree_size command runs the same verify-and-correct flow,
        # then checkpoints to persist the corrected metadata.
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.populate()

        real_size = self.get_ckpt_size()
        self.set_ckpt_size(1)

        with self.customStdoutPattern(
                lambda output: self.assertRegex(output, self.correcting_pattern)):
            report = wiredtiger.wiredtiger_repair(
                self.conn, f'fix_btree_size=(uri="{self.stable_uri}")')
        self.assertIn(f'fix_btree_size: verifying {self.stable_uri}', report)
        self.assertIn('checkpointing to persist corrections', report)
        self.assertNotIn('Failed', report)
        self.assertEqual(self.get_ckpt_size(), real_size,
            "repair fix_btree_size should restore the size derived from the tree")

    def test_fix_btree_size_no_uri_all_files(self):
        # With no uri, repair must walk the metadata and verify every stable file, not just
        # the first one. Create several tables, corrupt each checkpoint size differently, and
        # confirm all of them are verified and corrected.
        ntables = 3
        uris = [f'layered:{self.uri_base}_{i}' for i in range(ntables)]
        stable_uris = [f'file:{self.uri_base}_{i}.wt_stable' for i in range(ntables)]

        real_sizes = []
        for uri, stable_uri in zip(uris, stable_uris):
            self.session.create(uri, 'key_format=S,value_format=S')
            self.populate(uri)
            real_sizes.append(self.get_ckpt_size(stable_uri))

        # Corrupt each table with a distinct bogus size.
        for i, stable_uri in enumerate(stable_uris):
            self.set_ckpt_size(1 + i, stable_uri)

        with self.customStdoutPattern(
                lambda output: self.assertRegex(output, self.correcting_pattern)):
            report = wiredtiger.wiredtiger_repair(self.conn, 'fix_btree_size=()')

        # Every stable file must appear in the report and get its size corrected.
        for i, stable_uri in enumerate(stable_uris):
            self.assertIn(f'fix_btree_size: verifying {stable_uri}', report)
            self.assertEqual(self.get_ckpt_size(stable_uri), real_sizes[i],
                f"no-uri repair should correct {stable_uri}")

    def test_fix_btree_size_requires_leader(self):
        # fix_btree_size is rejected on a follower. The follower skips verify until it has picked
        # up a checkpoint, so the table must be checkpointed first.
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.populate()

        self.conn.reconfigure('disaggregated=(role="follower")')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.verify(self.uri, 'fix_btree_size=true'),
            '/requires a disaggregated leader connection/')

    def test_fix_btree_size_requires_writable(self):
        # fix_btree_size is rejected on a read-only connection. Read-only disaggregated
        # connections are rejected at open, so use a plain standalone database.
        dirname = 'test_fix_btree_size_readonly'
        os.mkdir(dirname)
        conn = self.wiredtiger_open(dirname, 'create')
        conn.open_session().create('table:t', 'key_format=S,value_format=S')
        conn.close()

        conn = self.wiredtiger_open(dirname, 'readonly=true')
        session = conn.open_session()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: session.verify('table:t', 'fix_btree_size=true'),
            '/requires a writable connection/')
        conn.close()

if __name__ == "__main__":
    wttest.run()

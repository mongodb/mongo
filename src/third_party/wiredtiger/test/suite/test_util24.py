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

import json

from helper_wt_corruption import WtCliMixin
import wttest

class test_util_read_corrupt(wttest.WiredTigerTestCase, WtCliMixin):
    leaf_uri = 'test_util_read_corrupt_leaf'
    root_uri = 'test_util_read_corrupt_root'
    nrows = 1000

    def _key(self, i):
        return f'k{i:08}'

    def _value(self, i):
        return f'v{i:08}' + 'x' * 1000

    # Keys hidden by the corrupted leaf - used to pin read tests to unreachable keys.
    def _missing_keys(self, leaf):
        stdout, _ = self._run_wt('-q', 'dump', leaf.uri, expect_failure=True)
        return [self._key(i) for i in range(self.nrows) if (self._key(i) + '\\00') not in stdout]

    # Without -q, corrupt-leaf dump panics.
    def test_dump_without_q(self):
        leaf = self.corrupt_btree(self.leaf_uri, target='leaf',
                                  key=self._key, value=self._value, nrows=self.nrows)
        _, stderr = self._run_wt('dump', leaf.uri, expect_failure=True)
        self.assertIn('WT_PANIC', stderr)

    # With -q, corrupt-leaf dump emits partial output and exits non-zero.
    def test_dump_with_q_corrupt_page(self):
        leaf = self.corrupt_btree(self.leaf_uri, target='leaf',
                                  key=self._key, value=self._value, nrows=self.nrows)
        stdout, stderr = self._run_wt('-q', 'dump', leaf.uri, expect_failure=True)
        self.assertNotIn('WT_PANIC', stderr)

        self.assertIn(self._key(0) + '\\00\n', stdout)
        self.assertIn(self._key(self.nrows - 1) + '\\00\n', stdout)

        present = sum(1 for i in range(self.nrows)
                      if (self._key(i) + '\\00') in stdout)
        self.assertLess(present, self.nrows)
        self.assertGreater(present, self.nrows // 2)

    # With -q, corrupt-root dump emits no records and exits non-zero.
    def test_dump_with_q_corrupted_root(self):
        root = self.corrupt_btree(self.root_uri, target='root',
                                  key=self._key, value=self._value, nrows=self.nrows)
        stdout, stderr = self._run_wt('-q', 'dump', root.uri, expect_failure=True)
        self.assertNotIn('WT_PANIC', stderr)
        present = sum(1 for i in range(self.nrows)
                      if (self._key(i) + '\\00') in stdout)
        self.assertEqual(present, 0)

    # Two-table -q dump -j, one corrupt leaf, one corrupt root.
    def test_dump_with_q_and_json(self):
        # The single dump merges both tables' output into one stdout, so we need
        # to tell leaf and root records apart from each other.
        leaf_value = lambda i: f'leaf{i:08}' + 'x' * 1000
        root_value = lambda i: f'root{i:08}' + 'x' * 1000

        leaf = self.corrupt_btree(self.leaf_uri, target='leaf',
                                  key=self._key, value=leaf_value, nrows=self.nrows)
        root = self.corrupt_btree(self.root_uri, target='root',
                                  key=self._key, value=root_value, nrows=self.nrows)

        stdout, stderr = self._run_wt('-q', 'dump', '-j', leaf.uri, root.uri, expect_failure=True)
        self.assertNotIn('WT_PANIC', stderr)

        json.loads(stdout)

        # Leaf table: partial output with first and last values present.
        self.assertIn(leaf_value(0), stdout)
        self.assertIn(leaf_value(self.nrows - 1), stdout)
        leaf_present = sum(1 for i in range(self.nrows) if leaf_value(i) in stdout)
        self.assertLess(leaf_present, self.nrows)
        self.assertGreater(leaf_present, self.nrows // 2)

        # Root table: nothing readable.
        root_present = sum(1 for i in range(self.nrows) if root_value(i) in stdout)
        self.assertEqual(root_present, 0)

    # Without -q, reading a corrupt-leaf key panics.
    def test_read_without_q(self):
        leaf = self.corrupt_btree(self.leaf_uri, target='leaf',
                                  key=self._key, value=self._value, nrows=self.nrows)
        missing = self._missing_keys(leaf)
        self.assertTrue(missing)
        _, stderr = self._run_wt('read', leaf.uri, missing[0], expect_failure=True)
        self.assertIn('WT_PANIC', stderr)

    # With -q, reading clean + corrupt keys: clean values print, no panic.
    def test_read_with_q_corrupt_leaf(self):
        leaf = self.corrupt_btree(self.leaf_uri, target='leaf',
                                  key=self._key, value=self._value, nrows=self.nrows)
        missing = self._missing_keys(leaf)
        self.assertTrue(missing)
        stdout, stderr = self._run_wt(
            '-q', 'read', leaf.uri,
            self._key(0), missing[0], self._key(self.nrows - 1),
            expect_failure=True)
        self.assertNotIn('WT_PANIC', stderr)
        self.assertIn(self._value(0), stdout)
        self.assertIn(self._value(self.nrows - 1), stdout)

    # With -q + corrupt root, no key is readable but no panic.
    def test_read_with_q_corrupted_root(self):
        root = self.corrupt_btree(self.root_uri, target='root',
                                  key=self._key, value=self._value, nrows=self.nrows)
        _, stderr = self._run_wt('-q', 'read', root.uri,
                                 self._key(self.nrows // 2), expect_failure=True)
        self.assertNotIn('WT_PANIC', stderr)

    # Without -q, stat walks the table and panics on the corrupt leaf.
    def test_stat_without_q(self):
        leaf = self.corrupt_btree(self.leaf_uri, target='leaf',
                                  key=self._key, value=self._value, nrows=self.nrows)
        _, stderr = self._run_wt('stat', leaf.uri, expect_failure=True)
        self.assertIn('WT_PANIC', stderr)

    # With -q, stat tolerates the corrupt leaf, still emits stats, exits non-zero.
    def test_stat_with_q_corrupt_leaf(self):
        leaf = self.corrupt_btree(self.leaf_uri, target='leaf',
                                  key=self._key, value=self._value, nrows=self.nrows)
        stdout, stderr = self._run_wt('-q', 'stat', leaf.uri, expect_failure=True)
        self.assertNotIn('WT_PANIC', stderr)
        self.assertTrue(stdout.strip())

if __name__ == '__main__':
    wttest.run()

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
from helper_disagg import DisaggCorruptionMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_disagg_corruption_mixin.py
#    Exercise the DisaggCorruptionMixin helpers against a palite-backed
#    disaggregated database.
@disagg_test_class
class test_disagg_corruption_mixin(wttest.WiredTigerTestCase, DisaggCorruptionMixin):
    def conn_config(self):
        return self.extensionsConfig() + ',create,disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages('test_disagg_corruption_mixin', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = 'layered:test_corruption_mixin'
    nentries = 10

    def _populate(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        c = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nentries):
            c[f'k{i:04d}'] = f'v{i:04d}'
        c.close()
        self.session.checkpoint()

    def test_corrupt_random_page_image(self):
        if self.ds_name != 'palite':
            self.skipTest('palite-only test')
        self._populate()
        table_id, page_id, lsn = self.corrupt_random_page_image()
        after = self.sqlite_select_json(table_id,
            f'SELECT hex(substr(page_data, 1, 1)) AS first FROM pages '
            f'WHERE table_id={table_id} AND page_id={page_id} AND lsn={lsn};')
        self.assertEqual(after[0]['first'], 'FF')

    def test_delete_random_page_image(self):
        if self.ds_name != 'palite':
            self.skipTest('palite-only test')
        self._populate()
        table_id, page_id, lsn = self.delete_random_page_image()
        after = self.sqlite_select_json(table_id,
            f'SELECT COUNT(*) AS n FROM pages '
            f'WHERE table_id={table_id} AND page_id={page_id} AND lsn={lsn};')
        self.assertEqual(after[0]['n'], 0)

    def test_set_random_page_discarded(self):
        if self.ds_name != 'palite':
            self.skipTest('palite-only test')
        self._populate()
        table_id, page_id, lsn = self.set_random_page_discarded()
        mask = DisaggCorruptionMixin.WT_PAGE_LOG_DISCARDED
        after = self.sqlite_select_json(table_id,
            f'SELECT discarded, flags FROM pages '
            f'WHERE table_id={table_id} AND page_id={page_id} AND lsn={lsn};')
        self.assertEqual(after[0]['discarded'], 1)
        self.assertEqual(int(after[0]['flags']) & mask, mask)

    def test_truncate_random_delta_chain(self):
        if self.ds_name != 'palite':
            self.skipTest('palite-only test')
        self._populate()

        # Apply a series of modifications to create page deltas.
        for iteration in range(5):
            c = self.session.open_cursor(self.uri, None, None)
            for i in range(self.nentries):
                c[f'k{i:04d}'] = f'v{i:04d}-{iteration}'
            c.close()
            self.session.checkpoint()

        table_id, page_id, kept_lsn, deleted_lsns = self.truncate_random_delta_chain()
        self.assertGreaterEqual(len(deleted_lsns), 1)
        remaining = [int(r['lsn']) for r in self.sqlite_select_json(table_id,
            f'SELECT lsn FROM pages '
            f'WHERE table_id={table_id} AND page_id={page_id} ORDER BY lsn;')]
        self.assertEqual(remaining, [kept_lsn])

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
from helper_disagg import DisaggConfigMixin, disagg_test_class
from wiredtiger import stat

# Reading compressed pages from disaggregated storage updates the compression-ratio read histogram.
@disagg_test_class
class test_disagg_compression_ratio01(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true),' \
        'statistics=(all),' \
        'cache_size=512MB'

    uri = 'table:disagg_compression_ratio'
    nrows = 2000
    value = 'x' * 800

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('compressors', 'snappy')
        DisaggConfigMixin.conn_extensions(self, extlist)

    def test_compression_ratio_read_histogram(self):
        # Configure a leaf page max sufficiently larger than the allocation size to allow compression.
        self.session.create(
            self.uri,
            'key_format=S,value_format=S,'
            'block_compressor=snappy,'
            'block_manager=disagg,type=file,'
            'leaf_page_max=32KB'
        )

        with wttest.open_cursor(self.session, self.uri) as cursor:
            for i in range(self.nrows):
                with self.transaction(commit_timestamp=i + 1):
                    cursor[f'key{i:08d}'] = self.value

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(self.nrows)}')
        self.session.checkpoint()

        # Reopen without wiping the page store, so every page is read back from disaggregated storage.
        self.conn_config = self.conn_config.replace(',lose_all_my_data=true', '')
        self.reopen_conn()

        with wttest.open_cursor(self.session, self.uri) as cursor:
            count = 0
            for k, v in cursor:
                self.assertEqual(v, self.value)
                count += 1
            self.assertEqual(count, self.nrows)

        with wttest.open_cursor(self.session, f'statistics:{self.uri}',
                                config='statistics=(all)') as cursor:
            hist_2 = cursor[stat.dsrc.compress_read_ratio_hist_2][2]
            hist_4 = cursor[stat.dsrc.compress_read_ratio_hist_4][2]
            hist_8 = cursor[stat.dsrc.compress_read_ratio_hist_8][2]
            hist_16 = cursor[stat.dsrc.compress_read_ratio_hist_16][2]
            hist_32 = cursor[stat.dsrc.compress_read_ratio_hist_32][2]
            hist_64 = cursor[stat.dsrc.compress_read_ratio_hist_64][2]
            hist_max = cursor[stat.dsrc.compress_read_ratio_hist_max][2]

        self.pr(
            f'compress_read histogram: hist_2={hist_2} hist_4={hist_4} hist_8={hist_8} '
            f'hist_16={hist_16} hist_32={hist_32} hist_64={hist_64} hist_max={hist_max}'
        )

        hist_ge4 = hist_4 + hist_8 + hist_16 + hist_32 + hist_64 + hist_max
        self.assertGreater(hist_ge4, 0)

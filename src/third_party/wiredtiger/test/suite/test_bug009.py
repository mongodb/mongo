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
# test_bug009.py
#       check that reconciliation takes into account prefix compression
#       when figuring out how to split pages
#

import wttest

class test_bug009(wttest.WiredTigerTestCase):
    name = 'test_bug009'
    uri = 'file:' + name

    def test_reconciliation_prefix_compression(self):
        # Configure 4KB pages with prefix compression enabled and support for
        # large data items.
        self.session.create(self.uri,
                'prefix_compression=1,' +
                'key_format=S,value_format=S,' +
                'internal_page_max=4KB,leaf_page_max=4KB,' +
                'leaf_value_max=3096')

        cursor = self.session.open_cursor(self.uri, None)
        # Insert two items with keys that will be prefix compressed and data
        # items sized so that the compression size difference tips the
        # size over a page boundary.
        cursor['fill_2__b_27'] = '0' * 2294
        cursor['fill_2__b_28'] = '0' * 3022

if __name__ == '__main__':
    wttest.run()

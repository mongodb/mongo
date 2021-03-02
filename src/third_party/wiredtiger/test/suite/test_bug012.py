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

import wiredtiger, wttest
from wtdataset import ComplexDataSet

# test_bug012.py
class test_bug012(wttest.WiredTigerTestCase):

    # Test that we detect illegal collators.
    def test_illegal_collator(self):
        msg = '/unknown collator/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.create('table:A', 'collator="xyzzy"'), msg)

    # Test we detect illegal key and value formats. Key and value formats are
    # one configuration we can expect to fail when incorrectly specified as
    # part of an LSM configuration, so test that path too. (Unknown collators
    # compressors and other extensions won't fail when configured to LSM as we
    # cannot know what extensions will be loaded when the LSM file is actually
    # created.)
    #
    # Test that we detect illegal key formats.
    def test_illegal_key_format(self):
        msg = '/Invalid type/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create('table:A', 'key_format="xyzzy"'), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create('table:A',
                'type=lsm,lsm=(bloom_config=(key_format="xyzzy"))'), msg)

    # Test that we detect illegal value formats.
    def test_illegal_value_format(self):
        msg = '/Invalid type/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create('table:A', 'value_format="xyzzy"'), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create('table:A',
                'type=lsm,lsm=(bloom_config=(value_format="xyzzy"))'), msg)

    # Test that we detect illegal compressors.
    def test_illegal_compressor(self):
        msg = '/unknown compressor/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.create('table:A', 'block_compressor="xyzzy"'), msg)

    # Test that we detect illegal extractors.
    #
    # This test is a little fragile, we're depending on ComplexDataSet to do
    # the heavy-lifting, so if that function changes, this could break.
    def test_illegal_extractor(self):
        ds = ComplexDataSet(self, 'table:A', 10)
        ds.populate()
        msg = '/unknown extractor/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.create('index:A:xyzzy',
            'key_format=S,columns=(column2),extractor="xyzzy"'), msg)

if __name__ == '__main__':
    wttest.run()

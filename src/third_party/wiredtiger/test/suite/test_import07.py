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
# test_import07.py
# Check that importing unsupported data sources returns an error.

import wiredtiger
from test_import01 import test_import_base
from wtscenario import make_scenarios

class test_import07(test_import_base):
    original_db_file = 'original_db_file'
    create_config = 'allocation_size=512,key_format=u,value_format=u'

    ntables = 10
    nrows = 100
    scenarios = make_scenarios([
        ('colgroup', dict(prefix='colgroup:')),
        ('lsm', dict(prefix='lsm:')),
        ('index', dict(prefix='index:')),
    ])

    def test_import_unsupported_data_source(self):
        # Make a bunch of files and fill them with data.
        self.populate(self.ntables, self.nrows)
        self.session.checkpoint()

        # Export the metadata for one of the files we made.
        # We just need an example of what a file configuration would typically look like.
        cursor = self.session.open_cursor('metadata:', None, None)
        for k, v in cursor:
            if k.startswith('table:'):
                example_db_file_config = cursor[k]
                break
        cursor.close()

        # Contruct the config string.
        import_config = 'import=(enabled,repair=false,file_metadata=(' + \
            example_db_file_config + '))'

        # This uri doesn't exist however, for the purposes of our test, this is fine.
        uri = self.prefix + self.original_db_file

        # Before importing, we check whether the data source is a 'file' or 'table'.
        # If not, we return ENOTSUP.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(uri, import_config),
            '/Operation not supported/')

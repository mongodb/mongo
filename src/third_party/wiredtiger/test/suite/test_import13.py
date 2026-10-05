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
# test_import13.py
#   A file whose internal page is addressed as a leaf by its parent is rejected.
#   Import verifies the file and fails cleanly; a file damaged after import fails
#   when read.

import os, shutil
import wiredtiger, wttest
from helper_wt_corruption import forge_internal_child_address_as_leaf, parse_verify_internal_pages
from suite_subprocess import suite_subprocess
from test_import01 import test_import_base

@wttest.skip_for_hook("disagg", "the test edits block offsets in a local data file")
class test_import13(test_import_base, suite_subprocess):
    conn_config = 'cache_size=50MB'

    source_uri = 'file:test_import13_source.wt'
    source_file = 'test_import13_source.wt'
    # Small pages and enough rows for internal pages whose children are internal
    # pages. checksum=off lets the forged cell sit outside the bytes the checksum covers.
    source_config = ('allocation_size=512,checksum=off,internal_page_max=512,'
        'leaf_page_max=512,key_format=S,value_format=S')
    row_count = 4000
    row_value = 'v' * 40
    mismatch = 'page type does not match reference type'

    def internal_pages(self, home, outfilename):
        self.runWt(['-h', home, 'verify', '-d', 'dump_address', self.source_uri],
            outfilename=outfilename, reopensession=False)
        with open(outfilename) as f:
            return parse_verify_internal_pages(f.read())

    def create_source(self):
        self.session.create(self.source_uri, self.source_config)
        cursor = self.session.open_cursor(self.source_uri)
        for key in range(self.row_count):
            cursor['key{:08d}'.format(key)] = self.row_value
        cursor.close()
        self.session.checkpoint()

        metadata = self.session.open_cursor('metadata:')
        file_config = metadata[self.source_uri]
        metadata.close()
        self.close_conn()
        return (file_config, self.internal_pages('.', 'verify_source.out'))

    def open_destination(self):
        newdir = 'IMPORT_DB'
        shutil.rmtree(newdir, ignore_errors=True)
        os.mkdir(newdir)
        self.conn = self.setUpConnectionOpen(newdir)
        self.session = self.setUpSessionOpen(self.conn)

        self.session.create('table:control', 'key_format=S,value_format=S')
        cursor = self.session.open_cursor('table:control')
        cursor['control_key'] = 'control_value'
        cursor.close()
        self.session.checkpoint()

        self.copy_file(self.source_file, '.', newdir)
        return newdir

    def import_file(self, file_config):
        import_config = 'import=(enabled,repair=false,file_metadata=({}))'.format(file_config)
        self.session.create(self.source_uri, import_config)

    def test_import_forged_file_rejected(self):
        file_config, pages = self.create_source()
        newdir = self.open_destination()
        imported_path = os.path.join(newdir, self.source_file)
        forge_internal_child_address_as_leaf(imported_path, pages)
        with open(imported_path, 'rb') as f:
            forged_image = f.read()

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.import_file(file_config), '/' + self.mismatch + '/')
        # The failed verify reports the file's extent lists.
        self.ignoreStdoutPatternIfExists('extent list')

        # The failed import leaves no metadata and does not touch the file.
        metadata = self.session.open_cursor('metadata:')
        metadata.set_key(self.source_uri)
        self.assertEqual(wiredtiger.WT_NOTFOUND, metadata.search())
        metadata.close()
        with open(imported_path, 'rb') as f:
            self.assertEqual(forged_image, f.read())

        cursor = self.session.open_cursor('table:control')
        cursor.set_key('control_key')
        self.assertEqual(0, cursor.search())
        self.assertEqual('control_value', cursor.get_value())
        cursor.close()

    def test_forged_after_import_rejected_on_read(self):
        file_config, _ = self.create_source()
        newdir = self.open_destination()
        self.import_file(file_config)
        self.close_conn()

        # Closing checkpoints the imported file, so find its pages again.
        imported_path = os.path.join(newdir, self.source_file)
        forge_internal_child_address_as_leaf(
            imported_path, self.internal_pages(newdir, 'verify_imported.out'))

        # Reading the forged page panics the connection, so read it from a
        # subprocess, and let the panic return instead of dumping core.
        self.runWt(['-h', newdir, '-C', 'debug_mode=(corruption_abort=false)', 'dump',
            self.source_uri], outfilename='dump.out', errfilename='dump.err', failure=True)
        self.check_file_contains('dump.err', self.mismatch)

    def test_import_valid_file(self):
        file_config, _ = self.create_source()
        newdir = self.open_destination()
        self.import_file(file_config)

        self.runWt(['-h', newdir, 'dump', self.source_uri], outfilename='dump.out')
        self.check_file_contains('dump.out', 'key00000000')

    def test_import_valid_file_in_transaction(self):
        file_config, _ = self.create_source()
        newdir = self.open_destination()

        # A running transaction makes the schema operation run on an internal session.
        self.session.begin_transaction()
        self.import_file(file_config)
        self.session.commit_transaction()

        self.runWt(['-h', newdir, 'dump', self.source_uri], outfilename='dump.out')
        self.check_file_contains('dump.out', 'key00000000')

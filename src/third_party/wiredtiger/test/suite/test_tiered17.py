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
# test_tiered17.py
#    Test that opening a file in readonly mode does not create a new object in tier.

from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources, get_conn_config
from wtscenario import make_scenarios
import fnmatch, os, wttest

class test_tiered17(TieredConfigMixin, wttest.WiredTigerTestCase):
    tiered_storage_sources = gen_tiered_storage_sources()
    saved_conn = ''
    uri = "table:test_tiered"

    shutdown = [
        ('clean', dict(clean=True)),
        ('unclean', dict(clean=False)),
    ]

    def conn_config(self):
        if self.is_tiered_scenario():
            self.saved_conn = get_conn_config(self) + ')'
        return self.saved_conn

    scenarios = make_scenarios(tiered_storage_sources, shutdown)

    def get_object_files(self):
        object_files = fnmatch.filter(os.listdir('.'), "*.wtobj") + fnmatch.filter(os.listdir('.'), '*.wt')
        return object_files

    def verify_checkpoint(self):
        obj_files_orig = self.get_object_files()
        ckpt_cursor = self.session.open_cursor(self.uri, None, 'checkpoint=WiredTigerCheckpoint')
        ckpt_cursor.close()
        obj_files = self.get_object_files()
        # Check that no additional object files have been created after opening the checkpoint.
        self.assertTrue(len(obj_files_orig) == len(obj_files))

    def populate(self):
        # Create and populate a table.
        self.session.create(self.uri, "key_format=S,value_format=S")
        c = self.session.open_cursor(self.uri)
        c["a"] = "a"
        c["b"] = "b"

        # Do a checkpoint and flush operation.
        self.session.checkpoint()
        self.session.flush_tier(None)

        # Add more data but don't do a checkpoint or flush in the unclean shutdown scenario.
        if not self.clean:
           c["c"] = "c"
           c["d"] = "d"
        c.close()

    def test_open_readonly_conn(self):
        self.populate()
        self.verify_checkpoint()
        obj_files_orig = self.get_object_files()

        # Re-open the connection but in readonly mode.
        conn_params = 'readonly=true,' + self.saved_conn
        self.reopen_conn(config = conn_params)

        obj_files = self.get_object_files()

        # Check that no additional object files have been created after re-opening the connection.
        self.assertTrue(len(obj_files_orig) == len(obj_files))

        self.close_conn()

        # Check that no additional object files have been created after closing the connection.
        obj_files = self.get_object_files()
        self.assertTrue(len(obj_files_orig) == len(obj_files))

    def test_open_readonly_cursor(self):
        self.populate()
        obj_files_orig = self.get_object_files()

        # Open the database in readonly mode.
        self.reopen_conn(config = self.saved_conn)
        c = self.session.open_cursor(self.uri, None, "readonly=true")

        obj_files = self.get_object_files()

        # Check that no additional object files have been created after re-opening the connection.
        self.assertTrue(len(obj_files_orig) == len(obj_files))

        c.close()
        self.close_conn()

        # Check that no additional object files have been created after closing the connection.
        obj_files = self.get_object_files()
        self.assertTrue(len(obj_files_orig) == len(obj_files))

if __name__ == '__main__':
    wttest.run()

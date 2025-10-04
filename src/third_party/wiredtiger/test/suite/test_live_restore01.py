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
import wiredtiger, wttest
import glob
import shutil
from wtbackup import backup_base

# test_live_restore01.py
# Test live restore compatibility with various other connection options.
class test_live_restore01(backup_base):

    def expect_success(self, config_str):
        self.open_conn("DEST", config=config_str)
        self.close_conn()

        # Clean out the destination. Subsequent live_restore opens will expect it to contain nothing.
        shutil.rmtree("DEST")
        os.mkdir("DEST")

    def expect_failure(self, config_str, expected_error):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.open_conn("DEST", config=config_str), expected_error)
        # Clean out the destination. Subsequent live_restore opens will expect it to contain nothing.
        shutil.rmtree("DEST")
        os.mkdir("DEST")

    def expect_failure_rounds(self, config_strs:list, interval_s:float, expected_error):
        for config_str in config_strs[:-1]:
            self.open_conn("DEST", config=config_str)
            if interval_s > 0:
                import time
                time.sleep(interval_s)
            self.close_conn()
        self.expect_failure(config_strs[-1], expected_error)
        shutil.rmtree("DEST")
        os.mkdir("DEST")

    def test_live_restore01(self):
        # Close the default connection.

        os.mkdir("SOURCE")
        self.take_full_backup("SOURCE")
        self.close_conn()

        # Remove everything but SOURCE / stderr / stdout.
        for f in glob.glob("*"):
            if not f == "SOURCE" and not f == "stderr.txt" and not f == "stdout.txt":
                os.remove(f)

        os.mkdir("DEST")

        # Test that live restore connection will fail on windows.
        if os.name == 'nt':
            self.expect_failure("live_restore=(enabled=true,path=SOURCE)", "/Live restore is not supported on Windows/")
            return

        # Open a valid connection.
        self.expect_success("live_restore=(enabled=true,path=SOURCE)")

        # Specify an in memory connection with live restore.
        self.expect_failure("in_memory=true,live_restore=(enabled=true,path=SOURCE)", "/Live restore is not compatible with an in-memory connection/")

        # Specify an in memory connection with live restore not enabled.
        self.expect_success("in_memory=true,live_restore=(enabled=false,path=SOURCE)")

        # Specify an empty path string.
        self.expect_failure("live_restore=(enabled=true,path=\"\")", "/No such file or directory/")

        # Specify a non existant path.
        self.expect_failure("live_restore=(enabled=true,path=\"fake.fake.fake\")", "/fake.fake.fake/")

        # Specify the max number of threads
        self.expect_success("live_restore=(enabled=true,path=SOURCE,threads_max=12)")

        # Specify one too many threads.
        self.expect_failure("live_restore=(enabled=true,path=SOURCE,threads_max=13)", "/Value too large for key/")

        # Specify the minimum allowed number of threads.
        self.expect_success("live_restore=(enabled=true,path=SOURCE,threads_max=0)")

        # Start in read only mode
        self.expect_failure("readonly=true,live_restore=(enabled=true,path=SOURCE)", "/live restore is incompatible with readonly mode/")

        # Specify salvage is enabled.
        self.expect_failure("salvage=true,live_restore=(enabled=true,path=SOURCE)", "/Live restore is not compatible with salvage/")

        # Specify statistics are disabled.
        self.expect_failure("statistics=(none),live_restore=(enabled=true,path=SOURCE)", "/Statistics must be enabled when live restore is active./")

        self.expect_failure("live_restore=(enabled=true,path=SOURCE),disaggregated=(page_log=palm)", "/Live restore is not compatible with disaggregated storage mode/")
        # Specify the multi round live_restore start and end
        # WT-15089
        self.expect_failure_rounds([
            "live_restore=(enabled=true,path=SOURCE,threads_max=0)",
            "live_restore=(enabled=false)"
        ], 0.1, "/Cannot start in non-live restore mode while a live restore is in progress!/")

        # Expect a failure if statistics are disabled via a reconfigure call.
        self.open_conn("DEST", config="live_restore=(enabled=true,path=SOURCE)")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure("statistics=(none)"), "/Statistics must be enabled when live restore is active./")
        self.close_conn()

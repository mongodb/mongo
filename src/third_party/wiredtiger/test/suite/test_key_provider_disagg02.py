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
import re, os, subprocess, json
import wttest
from run import wt_builddir
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_key_provider_disagg02.py
#    Ensure that a crash during checkpoint will not corrupt key provider meta information.
@disagg_test_class
class test_key_provider_disagg02(wttest.WiredTigerTestCase, suite_subprocess):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages('test_key_provider_disagg02', disagg_only = True)

    crash_points = [
        ('crash_before_key_rotation', dict(crash_point=1)),
        ('crash_during_key_rotation', dict(crash_point=2)),
        ('crash_after_key_rotation', dict(crash_point=3)),
    ]

    scenarios = make_scenarios(disagg_storages, crash_points)
    nentries = 1000
    uri = "layered:test_key_provider_disagg02"

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        config = f'=(early_load=true,config=\"verbose=-1,key_expires=0\")'
        extlist.extension('test', "key_provider" + config)
        DisaggConfigMixin.conn_extensions(self, extlist)

    def subprocess_func(self):
        self.dir = self.home
        # Populate table.
        ds = SimpleDataSet(self, self.uri, self.nentries)
        ds.populate()
        ds.check()

        # Initiate checkpoint to trigger key provider semantics.
        self.session.checkpoint()
        self.sqlite_fetch_shared_meta(write=True)

        # Trigger again and crash.
        self.session.checkpoint(f"debug=(key_provider_trigger_crash_points={self.crash_point})") # Expected to fail

    # Verify results of metadata file. After a crash, the key provider information should be the same.
    def validate_persist_meta_file(self):
        after_crash_meta = self.sqlite_fetch_shared_meta(write=False)
        pattern = (
            r"page_id=(?P<page_id>\d+),"
            r"lsn=(?P<lsn>\d+).*"
            r"version=(?P<version>\d+)"
        )
        after_crash_match = re.search(pattern, after_crash_meta)
        self.assertTrue(after_crash_match)

        result_file = os.path.join(self.dir, "key_provider.results")
        with open(result_file, "r") as f:
            before_crash_meta  = f.read()
            before_crash_match = re.search(pattern, before_crash_meta)
            self.assertEqual(before_crash_match.group("page_id"), after_crash_match.group("page_id"))
            self.assertEqual(before_crash_match.group("lsn"), after_crash_match.group("lsn"))
            self.assertEqual(before_crash_match.group("version"), after_crash_match.group("version"))

    # Fetch the latest metadata and perform read/write validation. Use the builtin sqlite3 to
    # match Palites SQLite version; some system SQLite builds are too old and may fail.
    def sqlite_fetch_shared_meta(self, write):
        sqlite_exe = os.path.join(wt_builddir, "sqlite3")
        database_home = os.path.join(self.dir, 'kv_home', 'pages_000001.db')
        result = subprocess.run(
            [sqlite_exe, "-json", database_home, "SELECT * FROM pages ORDER BY lsn DESC LIMIT 1;"],
            capture_output=True,
            text=True,
            check=True
        )
        result_data = json.loads(result.stdout)[0]
        result_file = os.path.join(self.dir, "key_provider.results")
        if (write):
            with open(result_file, "w") as f:
                f.write(result_data['page_data'])
        return result_data['page_data']

    def test_key_provider_disagg02(self):
        if (self.ds_name != "palite"):
            self.skipTest("Must use PALite to verify contents")

        self.conn.close()

        # Ensure that metadata file doesn't update key provider after crash.
        subdir = 'SUBPROCESS'
        [ignore_result, new_home_dir] = self.run_subprocess_function(subdir,
            'test_key_provider_disagg02.test_key_provider_disagg02.subprocess_func', silent=True)

        self.dir = new_home_dir
        self.validate_persist_meta_file()

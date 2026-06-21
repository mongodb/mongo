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
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_key_provider import KeyProviderBase
from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# Ensure that a crash during checkpoint will not corrupt key provider meta information.
@disagg_test_class
class test_key_provider_disagg02(KeyProviderBase, suite_subprocess):
    test_name = __qualname__
    disagg_storages = gen_disagg_storages(disagg_only = True)

    key_provider_versions = [
        ('pull', dict(key_provider_version=0)),
        ('push', dict(key_provider_version=1)),
    ]

    crash_points = [
        ('crash_before_key_rotation', dict(crash_point="before_key_rotation")),
        ('crash_during_key_rotation', dict(crash_point="during_key_rotation")),
        ('crash_after_key_rotation', dict(crash_point="after_key_rotation")),
    ]

    scenarios = make_scenarios(disagg_storages, key_provider_versions, crash_points)
    nentries = 1000
    uri = f"layered:{test_name}"

    # In push mode a checkpoint only persists a key once stable reaches its timestamp, so push at a
    # fresh timestamp and advance stable to it. Pull mode rotates through get_key, so it is a no-op.
    def rotate_key(self, timestamp):
        if self.key_provider_version != 1:
            return
        self.push_crypt_key(timestamp)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(timestamp))

    def subprocess_func(self):
        # Populate table.
        ds = SimpleDataSet(self, self.uri, self.nentries)
        ds.populate()
        ds.check()

        # Establish a durable baseline checkpoint that persists a key provider page.
        self.rotate_key(1)
        self.session.checkpoint()

        # Rotate again and crash mid-checkpoint.
        self.rotate_key(2)
        self.session.checkpoint(f"debug=(checkpoint_crash_trigger_point={self.crash_point})") # Expected to fail

    def test_key_provider_disagg02(self):
        self.conn.close()

        subdir = 'SUBPROCESS'
        [ignore_result, new_home_dir] = self.run_subprocess_function(subdir,
            f'{self.test_name}.{self.test_name}.subprocess_func', silent=True)

        # The subprocess wrote to new_home_dir; its turtle reference survived the crash intact.
        self.validate_turtle_page(home=new_home_dir)

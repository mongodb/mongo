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

# Publishing enters the disaggregated schema-epoch protocol, which is only meaningful once the
# stable schema epoch is set (epoch world). Publishing with no epoch set is a fatal protocol
# violation: it panics.

import os
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema18(wttest.WiredTigerTestCase, suite_subprocess, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true)'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def subprocess_publish_without_epoch_panics(self):
        """Subprocess body: publishing before the stable schema epoch is set panics."""
        self.session.create('layered:before', 'key_format=i,value_format=S')
        self.publish('layered:before', 10)  # Expected to panic.

    def test_publish_without_epoch_panics(self):
        """Publishing with no stable schema epoch set is a fatal protocol violation."""
        [returncode, home] = self.run_subprocess_function(
            'SUBPROCESS_publish_without_epoch_panics',
            f'{self.test_name}.{self.test_name}.subprocess_publish_without_epoch_panics',
            silent=True)
        self.assertNotEqual(returncode, 0)
        self.check_file_contains(os.path.join(home, 'stderr.txt'),
            'publish requires the stable disaggregated schema epoch to be set')

    def test_publish_with_epoch_succeeds(self):
        """With the stable epoch set first, create then publish is accepted."""
        self.set_stable_epoch(5)
        self.session.create('layered:after', 'key_format=i,value_format=S')
        self.publish('layered:after', 10)

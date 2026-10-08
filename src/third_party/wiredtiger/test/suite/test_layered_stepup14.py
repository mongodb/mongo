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

# Step-up must not drain and free a follower truncate entry that is still
# referenced by an open application transaction.

import os, signal, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios


@disagg_test_class
class test_layered_stepup14(wttest.WiredTigerTestCase, suite_subprocess):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    conn_base_config = ('statistics=(all),timing_stress_for_test=[disagg_role_transition],'
                        'disaggregated=(lose_all_my_data=true),')
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def setup_follower(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for key in range(10):
            cursor[key] = 'value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.conn.reconfigure('disaggregated=(role="follower")')

    def subprocess_uncommitted_follower_truncate(self):
        self.setup_follower()

        start = self.session.open_cursor(self.uri)
        stop = self.session.open_cursor(self.uri)
        start.set_key(2)
        stop.set_key(8)
        self.session.begin_transaction()
        self.session.truncate(None, start, stop, None)

        self.conn.reconfigure('disaggregated=(role="leader")')
        self.fail('step-up succeeded with an uncommitted follower truncate')

    def test_step_up_asserts_on_uncommitted_follower_truncate(self):
        rc, home = self.run_subprocess_function(
            'SUBPROCESS',
            'test_layered_stepup14.test_layered_stepup14.subprocess_uncommitted_follower_truncate',
            silent=True,
            scenario=self.scenario_name)
        self.assert_crashed(rc, signal.SIGABRT)
        with open(os.path.join(home, 'stderr.txt')) as err:
            self.assertIn(
              'an uncommitted follower truncate was found during the step-up drain', err.read())


if __name__ == '__main__':
    wttest.run()

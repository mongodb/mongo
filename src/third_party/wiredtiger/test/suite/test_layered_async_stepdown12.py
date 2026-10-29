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

# test_layered_async_stepdown12.py
#    A leader-era layered table whose stable constituent has gone missing must report the failed
#    open. Tolerating it would leave the cursor with no constituent at all: only a table created
#    inside the step-down window legitimately has no stable constituent, and such a table never
#    attempts the open.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_async_stepdown12(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__

    conn_config = 'precise_checkpoint=true,disaggregated=(role="leader")'
    table_config = 'key_format=S,value_format=S'
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # The open must fail rather than be tolerated on the strength of the step-down timestamp.
    def test_missing_stable_with_step_down_ts_set(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, self.table_config)
        # Reopen so the stable handle is not cached and its open consults the metadata.
        self.reopen_conn()

        # Open the cursor before the step-down timestamp is set, so the handle carries no window
        # mark and the first operation goes to the stable constituent.
        cursor = self.session.open_cursor(self.uri, None, None)

        metadata = self.session.open_cursor('file:WiredTiger.wt')
        metadata.set_key(self.stable_uri(self.uri))
        self.assertEqual(metadata.remove(), 0)
        metadata.close()

        self.session.begin_transaction()
        self.set_step_down_ts(20)
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.next(),
            '/No such file or directory/')
        self.session.rollback_transaction()
        cursor.close()

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

import wiredtiger, time, errno
from wttest import open_cursor
from error_info_util import error_info_util
from compact_util import compact_util

# test_error_info01.py
#   Test that the get_last_error() session API returns the last error to occur in the session.
class test_error_info01(error_info_util, compact_util):

    uri = "table:test_error_info01"

    def api_call_with_success(self):
        """
        Create a table, add a key, get it back.
        """
        self.session.create(self.uri, 'key_format=S,value_format=S')
        with open_cursor(self.session, self.uri) as inscursor:
            inscursor.set_key('key1')
            inscursor.set_value('value1')
            inscursor.insert()
        with open_cursor(self.session, self.uri) as getcursor:
            getcursor.set_key('key1')
            getcursor.search()

    def api_call_with_einval_wt_background_compaction_already_running(self):
        """
        Try to reconfigure background compaction while the bg compaction server is running.
        """
        new_bg_config = 'background=true,free_space_target=10MB'
        self.turn_on_bg_compact()
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: self.session.compact(None, new_bg_config))

    def api_call_with_ebusy_wt_uncommitted_data(self):
        """
        Try to drop a table while it still has uncommitted data.
        """
        self.session.create(self.uri, 'key_format=S,value_format=S')
        with open_cursor(self.session, self.uri) as cursor:
            self.session.begin_transaction()
            cursor.set_key('key')
            cursor.set_value('value')
            self.assertEqual(cursor.update(), 0)
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, None)), "was expecting drop call to fail with EBUSY")

    def api_call_with_ebusy_wt_dirty_data(self):
        """
        Try to drop a table without first performing a checkpoint.
        """
        self.session.create(self.uri, 'key_format=S,value_format=S')
        with open_cursor(self.session, self.uri) as cursor:
            self.session.begin_transaction()
            cursor.set_key('key')
            cursor.set_value('value')
            self.assertEqual(cursor.update(), 0)
            self.assertEqual(self.session.commit_transaction(), 0)

        # Give time for the oldest id to update before dropping the table.
        time.sleep(1)
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, None)), "was expecting drop call to fail with EBUSY")

    def test_success(self):
        self.api_call_with_success()
        self.assert_error_equal(0, wiredtiger.WT_NONE, "last API call was successful")

    def test_einval_wt_background_compaction_already_running(self):
        self.api_call_with_einval_wt_background_compaction_already_running()
        self.assert_error_equal(errno.EINVAL, wiredtiger.WT_BACKGROUND_COMPACT_ALREADY_RUNNING, "Cannot reconfigure background compaction while it's already running.")

    def test_ebusy_wt_uncommitted_data(self):
        self.api_call_with_ebusy_wt_uncommitted_data()
        self.assert_error_equal(errno.EBUSY, wiredtiger.WT_UNCOMMITTED_DATA, "the table has uncommitted data and cannot be dropped yet")
        self.assertEqual(self.session.rollback_transaction(), 0)
        self.assertEqual(self.session.checkpoint(), 0)
        self.assertEqual(self.session.drop(self.uri, None), 0)

    def test_ebusy_wt_dirty_data(self):
        self.api_call_with_ebusy_wt_dirty_data()
        self.assert_error_equal(errno.EBUSY, wiredtiger.WT_DIRTY_DATA, "the table has dirty data and can not be dropped yet")
        self.assertEqual(self.session.checkpoint(), 0)
        self.assertEqual(self.session.drop(self.uri, None), 0)

    def test_api_call_alternating(self):
        """
        Test that successive API calls correctly set the error codes and message. The stored
        codes/message should reflect the result of the most recent API call, regardless of whether
        it failed or succeeded.
        """
        self.assert_error_equal(0, wiredtiger.WT_NONE, "last API call was successful")
        self.test_success()
        self.test_einval_wt_background_compaction_already_running()
        self.test_ebusy_wt_uncommitted_data()
        self.test_ebusy_wt_dirty_data()
        self.test_success()
        self.test_ebusy_wt_dirty_data()
        self.test_ebusy_wt_uncommitted_data()
        self.test_einval_wt_background_compaction_already_running()
        self.test_success()

    def test_api_call_doubling(self):
        """
        Test that successive API calls with the same outcome result in the same error codes and
        message being stored. The codes/message should only change when the result changes.
        """
        self.assert_error_equal(0, wiredtiger.WT_NONE, "last API call was successful")
        self.test_success()
        self.test_success()
        self.test_einval_wt_background_compaction_already_running()
        self.test_einval_wt_background_compaction_already_running()
        self.test_ebusy_wt_uncommitted_data()
        self.test_ebusy_wt_uncommitted_data()
        self.test_ebusy_wt_dirty_data()
        self.test_ebusy_wt_dirty_data()

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
# [TEST_TAGS]
# session_api
# [END_TAGS]

import wiredtiger, wttest
from suite_subprocess import suite_subprocess

# test_strerror01.py
#     Test generation of sub-level error codes when using calling strerror.
class test_strerror(wttest.WiredTigerTestCase, suite_subprocess):
    sub_errors = [
        (wiredtiger.WT_NONE, "WT_NONE: No additional context"),
        (wiredtiger.WT_BACKGROUND_COMPACT_ALREADY_RUNNING, "WT_BACKGROUND_COMPACT_ALREADY_RUNNING: Background compaction is already running"),
        (wiredtiger.WT_CACHE_OVERFLOW, "WT_CACHE_OVERFLOW: Cache capacity has overflown"),
        (wiredtiger.WT_WRITE_CONFLICT, "WT_WRITE_CONFLICT: Write conflict between concurrent operations"),
        (wiredtiger.WT_OLDEST_FOR_EVICTION, "WT_OLDEST_FOR_EVICTION: Transaction has the oldest pinned transaction ID"),
        (wiredtiger.WT_CONFLICT_BACKUP, "WT_CONFLICT_BACKUP: Conflict performing operation due to running backup"),
        (wiredtiger.WT_CONFLICT_DHANDLE, "WT_CONFLICT_DHANDLE: Another thread currently holds the data handle of the table"),
        (wiredtiger.WT_CONFLICT_SCHEMA_LOCK, "WT_CONFLICT_SCHEMA_LOCK: Conflict performing schema operation"),
        (wiredtiger.WT_UNCOMMITTED_DATA, "WT_UNCOMMITTED_DATA: Table has uncommitted data"),
        (wiredtiger.WT_DIRTY_DATA, "WT_DIRTY_DATA: Table has dirty data"),
        (wiredtiger.WT_CONFLICT_TABLE_LOCK, "WT_CONFLICT_TABLE_LOCK: Another thread currently holds the table lock"),
        (wiredtiger.WT_CONFLICT_CHECKPOINT_LOCK, "WT_CONFLICT_CHECKPOINT_LOCK: Another thread currently holds the checkpoint lock"),
        (wiredtiger.WT_CONFLICT_LIVE_RESTORE, "WT_CONFLICT_LIVE_RESTORE: Conflict performing operation due to an in-progress live restore"),
        (wiredtiger.WT_CONFLICT_DISAGG, "WT_CONFLICT_DISAGG: Conflict with disaggregated storage"),
    ]

    def check_error_code(self, error, expected):
        assert self.session.strerror(error) == expected

    def test_strerror(self):
       for code, expected in self.sub_errors:
           self.check_error_code(code, expected)

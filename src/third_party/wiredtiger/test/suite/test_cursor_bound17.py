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

import wiredtiger, wttest
from wtscenario import make_scenarios
from wtbound import bound_base

# test_cursor_bound17.py
#   Test cursor bound reset functionality with special internal reset scenario calls.
class test_cursor_bound17(bound_base):
    file_name = 'test_cursor_bound17'

    types = [
        ('file', dict(uri='file:', use_colgroup=False)),
        ('table', dict(uri='table:', use_colgroup=False)),
        ('colgroup', dict(uri='table:', use_colgroup=True))
    ]

    key_formats = [
        ('string', dict(key_format='S')),
        ('var', dict(key_format='r')),
        ('int', dict(key_format='i')),
        ('bytes', dict(key_format='u')),
        ('composite_string', dict(key_format='SSS')),
        ('composite_int_string', dict(key_format='iS')),
        ('composite_complex', dict(key_format='iSru')),
    ]

    value_formats = [
        ('string', dict(value_format='S')),
        ('complex-string', dict(value_format='SS')),
    ]

    config = [
        ('evict', dict(evict=True)),
        ('no-evict', dict(evict=False))
    ]
    scenarios = make_scenarios(config, types, key_formats, value_formats)

    def test_bound_checkpoint_or_rollback(self):
        cursor = self.create_session_and_cursor()

        # Test bound api: Test that when checkpoint resets all cursors it doesn't clear the bounds 
        # on all the cursors.
        self.set_bounds(cursor, 30, "lower")
        self.set_bounds(cursor, 60, "upper")
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        self.session.checkpoint()
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        cursor.reset()

        # Test bound api: Test that when rollback resets all cursors it doesn't clear the bounds 
        # on all the cursors.
        self.session.begin_transaction()
        self.set_bounds(cursor, 30, "lower")
        self.set_bounds(cursor, 60, "upper")
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        self.session.rollback_transaction()
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        cursor.reset()

        # Test bound api: Test that when commit resets all cursors it doesn't clear the bounds 
        # on all the cursors.
        self.session.begin_transaction()
        self.set_bounds(cursor, 30, "lower")
        self.set_bounds(cursor, 60, "upper")
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        self.session.commit_transaction()
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        cursor.reset()

        # Test bound api: Test that when reconfigure resets all cursors it doesn't clear the bounds 
        # on all the cursors.
        self.set_bounds(cursor, 30, "lower")
        self.set_bounds(cursor, 60, "upper")
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        self.session.reconfigure("cache_cursors=false")
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        cursor.reset()

        # Test bound api: Test that when session reset api resets all cursors it doesn't clear the
        # bounds on all the cursors.
        self.set_bounds(cursor, 30, "lower")
        self.set_bounds(cursor, 60, "upper")
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        self.session.reset()
        self.cursor_traversal_bound(cursor, 30, 60, True)
        self.cursor_traversal_bound(cursor, 30, 60, False)
        cursor.reset()

        self.cursor_traversal_bound(cursor, None, None, True)
        self.cursor_traversal_bound(cursor, None, None, False)
        cursor.close()


if __name__ == '__main__':
    wttest.run()

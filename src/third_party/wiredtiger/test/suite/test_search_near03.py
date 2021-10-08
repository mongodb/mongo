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

import wiredtiger, re, wttest
from wtscenario import make_scenarios

# test_search_near03.py
# Test prefix search near format rules, only format fixed-length strings (s), variable strings (S)
# and raw bytes array (u) is allowed.
class test_search_near03(wttest.WiredTigerTestCase):
    key_format_values = [
         ('s', dict(key_format='s')),
         ('5s', dict(key_format='5s')),
         ('10s', dict(key_format='10s')),
         ('var S', dict(key_format='S')),
         ('u', dict(key_format='u')),
         ('ss', dict(key_format='ss')),
         ('us', dict(key_format='us')),
         ('10s10s', dict(key_format='10s10s')),
         ('S10s', dict(key_format='S10s')),
         ('u10s', dict(key_format='u10s')),
         ('var SS', dict(key_format='SS')),
         ('Su', dict(key_format='Su')),
    ]

    scenarios = make_scenarios(key_format_values)

    def valid_key_format(self):
        if self.key_format == 'u' or self.key_format == 'S' :
            return True
        if re.search('^\d*s$', self.key_format):
            return True
        return False

    def test_prefix_reconfigure(self):
        uri = 'table:test_search_near'
        self.session.create(uri, 'key_format={},value_format=S'.format(self.key_format))
        cursor = self.session.open_cursor(uri)
        # Check if the format is valid for prefix configuration.
        if self.valid_key_format():
            self.assertEqual(cursor.reconfigure("prefix_search=true"), 0)
        else:
            msg = '/Invalid argument/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: cursor.reconfigure("prefix_search=true"), msg)
        cursor.close()

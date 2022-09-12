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
import wttest
from wtscenario import make_scenarios
from wtbound import set_prefix_bound, bound_base

# test_cursor_bound15.py
# This test checks the edge case that a search_near call with bounds and prefix key
# will return the right value of exact. 
class test_cursor_bound15(bound_base):
    key_format_values = [
        ('var_string', dict(key_format='S')),
        ('byte_array', dict(key_format='u')),
    ]

    eviction = [
        ('eviction', dict(eviction=True)),
        ('no eviction', dict(eviction=False)),
    ]

    scenarios = make_scenarios(key_format_values, eviction)
    
    def check_key(self, key):
        if self.key_format == 'u':
            return key.encode()
        elif self.key_format == '10s':
            return key.ljust(10, "\x00")
        else:
            return key

    def test_cursor_bound(self):
        uri = 'table:test_cursor_bound'
        self.session.create(uri, 'key_format={},value_format=S'.format(self.key_format))
        cursor = self.session.open_cursor(uri)
        cursor2 = self.session.open_cursor(uri, None, "debug=(release_evict=true)")
        # Basic character array.
        l = "abcdefghijklmnopqrstuvwxyz"

        # Insert keys aaa -> aaz with timestamp 200.
        prefix = "aa"
        self.session.begin_transaction()
        for k in range (0, 25):
            key = prefix + l[k]
            cursor[key] = key
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(200))

        # Insert key aaz with timestamp 50.
        self.session.begin_transaction()
        cursor[prefix + "z"] = prefix + "z"
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(50))

        if self.eviction:
            # Evict the whole range.
            for k in range (0, 26):
                cursor2.set_key(prefix + l[k])
                self.assertEqual(cursor2.search(), 0)
                self.assertEqual(cursor2.reset(), 0)
        
        # Begin transaction at timestamp 250, all keys should be visible.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(250))
        cursor3 = self.session.open_cursor(uri)
        cursor3.reset()

        # Test with only lower bound set.
        self.assertEqual(self.set_bounds(cursor3, "aab", "lower"), 0) 
        cursor3.set_key("ab")
        self.assertEqual(cursor3.search_near(), -1)
        self.assertEqual(cursor3.get_key(), self.check_key("aaz"))
        cursor3.reset()

        self.assertEqual(self.set_bounds(cursor3, "aac", "lower"), 0) 
        cursor3.set_key("ab")
        self.assertEqual(cursor3.search_near(), -1)
        self.assertEqual(cursor3.get_key(), self.check_key("aaz"))
        cursor3.reset()

        self.assertEqual(self.set_bounds(cursor3, "aaz", "lower", True), 0) 
        cursor3.set_key("aaz")
        self.assertEqual(cursor3.search_near(), 0)
        self.assertEqual(cursor3.get_key(), self.check_key("aaz"))
        cursor3.reset()

        self.assertEqual(self.set_bounds(cursor3, "a", "lower"), 0) 
        cursor3.set_key("aa")
        self.assertEqual(cursor3.search_near(), 1)
        self.assertEqual(cursor3.get_key(), self.check_key("aaa"))
        cursor3.reset()

        # Test with only upper bound set.
        self.assertEqual(self.set_bounds(cursor3, "aac", "upper", True), 0) 
        cursor3.set_key("aad")
        self.assertEqual(cursor3.search_near(), -1)
        self.assertEqual(cursor3.get_key(), self.check_key("aac"))
        cursor3.reset()

        self.assertEqual(self.set_bounds(cursor3, "aaz", "upper"), 0) 
        cursor3.set_key("aac")
        self.assertEqual(cursor3.search_near(), 0)
        self.assertEqual(cursor3.get_key(), self.check_key("aac"))
        cursor3.reset()

        self.assertEqual(self.set_bounds(cursor3, "ac", "upper"), 0) 
        cursor3.set_key("aa")
        self.assertEqual(cursor3.search_near(), 1)
        self.assertEqual(cursor3.get_key(), self.check_key("aaa"))
        cursor3.reset()

        # Test with both bounds set.
        self.assertEqual(self.set_bounds(cursor3, "aaa", "lower"), 0) 
        self.assertEqual(self.set_bounds(cursor3, "aad", "upper"), 0) 
        cursor3.set_key("aae")
        self.assertEqual(cursor3.search_near(), -1)
        self.assertEqual(cursor3.get_key(), self.check_key("aad"))
        cursor3.reset()

        self.assertEqual(self.set_bounds(cursor3, "aaa", "lower"), 0) 
        self.assertEqual(self.set_bounds(cursor3, "aae", "upper"), 0) 
        cursor3.set_key("aad")
        self.assertEqual(cursor3.search_near(), 0)
        self.assertEqual(cursor3.get_key(), self.check_key("aad"))
        cursor3.reset()

        self.assertEqual(self.set_bounds(cursor3, "aac", "lower", True), 0) 
        self.assertEqual(self.set_bounds(cursor3, "aaz", "upper", True), 0) 
        cursor3.set_key("aab")
        self.assertEqual(cursor3.search_near(), 1)
        self.assertEqual(cursor3.get_key(), self.check_key("aac"))
        cursor3.reset()

        # Test with prefix bounds set.
        # Search near for aaza, with prefix bounds aaa should return the closest visible key: aaz.
        set_prefix_bound(self, cursor3, "aaz")
        self.session.breakpoint()
        cursor3.set_key("aaza")
        self.assertEqual(cursor3.search_near(), -1)
        self.assertEqual(cursor3.get_key(), self.check_key("aaz"))
        cursor3.reset()

        # Search near for ab, with prefix bounds "aaa" should return the closest visible key: aaa.
        set_prefix_bound(self, cursor3, "aaa")
        self.session.breakpoint()
        cursor3.set_key("ab")
        self.assertEqual(cursor3.search_near(), -1)
        self.assertEqual(cursor3.get_key(), self.check_key("aaa"))
        cursor3.reset()

        # Search near for aac, should return the closest visible key: aac.
        set_prefix_bound(self, cursor3, "a")
        self.session.breakpoint()
        cursor3.set_key("aac")
        self.assertEqual(cursor3.search_near(), 0)
        self.assertEqual(cursor3.get_key(), self.check_key("aac"))
        cursor3.reset()

        # Search near for aa, should return the closest visible key: aaa.
        set_prefix_bound(self, cursor3, "aaa")
        self.session.breakpoint()
        cursor3.set_key("aa")
        self.assertEqual(cursor3.search_near(), 1)
        self.assertEqual(cursor3.get_key(), self.check_key("aaa"))
        cursor3.reset()

        cursor3.close()
        self.session.commit_transaction()

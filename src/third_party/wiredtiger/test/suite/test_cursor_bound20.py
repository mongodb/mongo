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

# test_cursor_bound20.py
#    Test special cursor index bounds case. This python test aims to test the edge cases of
# increment_bounds_array function. When max values are given as a key, it will not increment and
# can turn off the upper bounds setting in some edge cases.
class test_cursor_bound20(bound_base):
    file_name = 'test_cursor_bound20'
    uri = 'table:'
    key_format = 'S'

    def set_bounds(self, cursor, key, bound_config, inclusive = None):
        inclusive_config = ""
        if (bound_config == "lower"):
            if (inclusive == False):
                inclusive_config = ",inclusive=false"
        elif (bound_config == "upper"):
            if (inclusive == False):
                inclusive_config = ",inclusive=false"

        # Set key and bounds.    
        cursor.set_key(key)
        return cursor.bound("bound={0}{1}".format(bound_config, inclusive_config))

    def gen_uval(self, i):
        result = bytearray()
        while i > 0:
            n = i % 4
            i = i // 4
            if n == 0:
                pass
            elif n == 3:
                result.insert(0, 0xff)
            else:
                result.insert(0, n - 1)   # either 00 or 01
        return bytes(result)    

    def gen_index_table(self):
        # Test Index index_cursors bound API support.
        suburi = "index:" + self.file_name + ":i0"
        start = 0
        columns_param = "columns=("
        for v in self.value_format:
            if v.isdigit():
                continue

            columns_param += "v{0},".format(str(start)) 
            start += 1
        columns_param += ")"
        self.session.create(suburi, columns_param)


    def test_cursor_index_bounds_fixed(self):
        self.value_format = '4s'
        MAX_FIXED_STRING = chr(127) + chr(127) + chr(127) + chr(127)

        uri = self.uri + self.file_name
        create_params = 'value_format={},key_format={}'.format(self.value_format, self.key_format)
        create_params += self.gen_create_param()
        self.session.create(uri, create_params)

        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        for i in range(self.start_key, self.end_key + 1):
            cursor[self.gen_key(i)] = MAX_FIXED_STRING
        self.session.commit_transaction()
        cursor.close()

        # Create index table.
        self.gen_index_table()
        index_cursor = self.session.open_cursor("index:" + self.file_name + ":i0")

        # Set bounds at lower key and upper max value. This is to validate the increment bounds 
        # function for fixed length string.
        self.set_bounds(index_cursor, "0000", "lower")
        self.set_bounds(index_cursor, MAX_FIXED_STRING, "upper")
        self.cursor_traversal_bound(index_cursor, None, None, True)
        self.cursor_traversal_bound(index_cursor, None, None, False)
        
        # Test basic search near scenarios.
        index_cursor.set_key(MAX_FIXED_STRING)
        self.assertEqual(index_cursor.search_near(), 0)
        self.assertEqual(index_cursor.get_key(), self.check_key(MAX_FIXED_STRING))

        # Test basic search scnarios.
        index_cursor.set_key(MAX_FIXED_STRING)
        self.assertEqual(index_cursor.search(), 0)
        self.assertEqual(index_cursor.get_key(), self.check_key(MAX_FIXED_STRING))
        index_cursor.reset()
        
        # Test index case: Lower bound with exclusive
        self.set_bounds(index_cursor, MAX_FIXED_STRING, "lower", False)
        self.cursor_traversal_bound(index_cursor, None, None, True, 0)
        self.cursor_traversal_bound(index_cursor, None, None, False, 0)
        
        index_cursor.set_key(MAX_FIXED_STRING)
        self.assertEqual(index_cursor.search_near(), wiredtiger.WT_NOTFOUND)

        index_cursor.set_key(MAX_FIXED_STRING)
        self.assertEqual(index_cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_cursor_index_bounds_byte(self):
        self.value_format = '2u'
        MAX_BYTE_ARRAY = bytearray()
        MAX_BYTE_ARRAY.insert(0, 0xFF)
        MAX_BYTE_ARRAY.insert(0, 0xFF)
        MAX_BYTE_ARRAY = bytes(MAX_BYTE_ARRAY)

        uri = self.uri + self.file_name
        create_params = 'value_format=u,key_format={}'.format(self.key_format)
        create_params += self.gen_create_param()
        self.session.create(uri, create_params)

        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        count = 0
        for i in range(self.start_key, self.end_key + 1):
            cursor[self.gen_key(i)] = self.gen_uval(count)
            count = (count + 1) % 20
        self.session.commit_transaction()
        cursor.close()

        # Create index table.
        self.gen_index_table()
        index_cursor = self.session.open_cursor("index:" + self.file_name + ":i0")

        # Set bounds at lower key and upper max byte value. This is to validate the increment bounds 
        # function for bytes.
        self.set_bounds(index_cursor, bytes(0), "lower")
        self.set_bounds(index_cursor, MAX_BYTE_ARRAY, "upper")
        self.cursor_traversal_bound(index_cursor, None, None, True)
        self.cursor_traversal_bound(index_cursor, None, None, False)
        
        # Test basic search near scenarios.
        index_cursor.set_key(MAX_BYTE_ARRAY)
        self.assertEqual(index_cursor.search_near(), 0)

        # Test basic search scnarios.
        index_cursor.set_key(MAX_BYTE_ARRAY)
        self.assertEqual(index_cursor.search(), 0)
        self.assertEqual(index_cursor.get_key(), MAX_BYTE_ARRAY)
        index_cursor.reset()
        
        # Test index case: Lower bound with exclusive
        self.set_bounds(index_cursor, MAX_BYTE_ARRAY, "lower", False)
        self.cursor_traversal_bound(index_cursor, None, None, True, 0)
        self.cursor_traversal_bound(index_cursor, None, None, False, 0)
        
        index_cursor.set_key(MAX_BYTE_ARRAY)
        self.assertEqual(index_cursor.search_near(), wiredtiger.WT_NOTFOUND)

        index_cursor.set_key(MAX_BYTE_ARRAY)
        self.assertEqual(index_cursor.search(), wiredtiger.WT_NOTFOUND)

if __name__ == '__main__':
    wttest.run()

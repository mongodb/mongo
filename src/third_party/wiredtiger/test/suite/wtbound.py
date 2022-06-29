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

# Shared base class used by cursor bound tests.
class bound_base(wttest.WiredTigerTestCase):
    start_key = 20
    end_key = 80
    lower_inclusve = True
    upper_inclusive = True

    def gen_colgroup_create_param(self):
        create_params = ",columns=("
        start = 0
        for _ in self.key_format:
            create_params += "k{0},".format(str(start)) 
            start += 1
        create_params += "v),colgroups=(g0)"
        return create_params

    def gen_key(self, i):
        tuple_key = []
        for key in self.key_format:
            if key == 'S' or key == 'u':
                tuple_key.append(str(i))
            elif key == "r":
                tuple_key.append(self.recno(i))
            elif key == "i":
                tuple_key.append(i)
        
        if (len(self.key_format) == 1):
            return tuple_key[0]
        else:
            return tuple(tuple_key)

    def check_key(self, i):
        list_key = []
        for key in self.key_format:
            if key == 'S':
                list_key.append(str(i))
            elif key == "r":
                list_key.append(self.recno(i))
            elif key == "i":
                list_key.append(i)
            elif key == "u":
                list_key.append(str(i).encode())
        
        if (len(self.key_format) == 1):
            return list_key[0]
        else:
            return list_key

    def set_bounds(self, cursor, key, bound_config, inclusive = None):
        inclusive_config = ""
        if (bound_config == "lower"):
            if (inclusive == False):
                inclusive_config = ",inclusive=false"
                self.lower_inclusive = False
            else:
                self.lower_inclusive = True
        elif (bound_config == "upper"):
            if (inclusive == False):
                inclusive_config = ",inclusive=false"
                self.upper_inclusive = False
            else:
                self.upper_inclusive = True

        # Set key and bounds.    
        cursor.set_key(self.gen_key(key))
        return cursor.bound("bound={0}{1}".format(bound_config, inclusive_config))
    
    def cursor_traversal_bound(self, cursor, lower_key, upper_key, next=None, expected_count=None):
        if next == None:
            next = self.direction

        start_range = self.start_key
        end_range = self.end_key

        if (upper_key):
            if (upper_key < end_range):
                end_range = upper_key
                if (self.upper_inclusive == False):
                    end_range -= 1
        
        if (lower_key):
            if (lower_key > start_range):
                start_range = lower_key
                if (self.lower_inclusive == False):
                    start_range += 1

        count = ret = 0
        while True:
            self.session.breakpoint()
            if (next):
                ret = cursor.next()
            else:
                ret = cursor.prev()
            self.assertTrue(ret == 0 or ret == wiredtiger.WT_NOTFOUND)
            if ret == wiredtiger.WT_NOTFOUND:
                break
            count += 1
            key = cursor.get_key()
            
            if (self.lower_inclusive and lower_key):
                self.assertTrue(self.check_key(lower_key) <= key)
            elif (lower_key):
                # print("lower key:")
                # print(lower_key)
                # print(key)
                self.assertTrue(self.check_key(lower_key) < key)
                
            if (self.upper_inclusive and upper_key):
                self.assertTrue(key <= self.check_key(upper_key))
            elif (upper_key):
                self.assertTrue(key < self.check_key(upper_key))
                
        count = max(count - 1, 0)
        if (expected_count != None):
            self.assertEqual(expected_count, count)
        else:
            self.assertEqual(end_range - start_range, count)

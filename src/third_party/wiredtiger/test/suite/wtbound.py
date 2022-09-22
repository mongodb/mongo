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

def set_prefix_bound(test, cursor, lower_bound):
    test.assertEqual(cursor.reset(), 0)
    cursor.set_key(lower_bound)
    test.assertEqual(cursor.bound("action=set,bound=lower,inclusive=true"), 0)
    # Strings are immutable in python so we cast the string to a list, then we convert the last
    # element to an integer using ord() and then convert it back to a char with chr(), finally we
    # convert the list back to a string.
    upper_bound = list(lower_bound)
    upper_bound[len(upper_bound) - 1] = chr(ord(upper_bound[len(upper_bound) - 1]) + 1)
    upper_bound = ''.join(upper_bound)
    cursor.set_key(upper_bound)
    test.assertEqual(cursor.bound("action=set,bound=upper,inclusive=false"), 0)

class bound():
    def __init__(self, key, inclusive, enabled):
        self.key = key
        self.inclusive = inclusive
        self.enabled = enabled
        self.key_format = 'S'
        self.value_format = 'S'

    def to_string(self):
        return "Enabled: " + str(self.enabled) + ", Key: " + str(self.key) + ", incl: " + self.inclusive_str()

    def inclusive_str(self):
        if (self.inclusive):
            return "true"
        else:
            return "false"

class bounds():
    # Initialize with junk values.
    lower = bound(-1, False, False)
    upper = bound(-1, False, False)

    def __init__(self, lower, upper):
        self.lower = lower
        self.upper = upper

    def to_string(self):
        return "Lower: [" + self.lower.to_string() + "], Upper: [" + self.upper.to_string() + "]"

    def in_bounds_key(self, key):
        if (self.lower.enabled):
            if (key == self.lower.key):
                if (not self.lower.inclusive):
                    return False
            elif (key < self.lower.key):
                return False
        if (self.upper.enabled):
            if (key == self.upper.key):
                if (not self.upper.inclusive):
                    return False
            elif (key > self.upper.key):
                return False
        return True

    # This is used by for loops, so add one to the expected end range.
    def end_range(self, max_key):
        if (not self.upper.enabled):
            return max_key
        if (self.upper.inclusive):
            return self.upper.key + 1
        return self.upper.key

    def start_range(self, min_key):
        if (not self.lower.enabled):
            return min_key
        if (self.lower.inclusive):
            return self.lower.key
        return self.lower.key + 1

# Shared base class used by cursor bound tests.
class bound_base(wttest.WiredTigerTestCase):
    # The start and end key denotes the first and last key in the table. Since 20 is a key itself, 
    # there are 60 entries between the start and end key.
    start_key = 20
    end_key = 79
    lower_inclusive = True
    upper_inclusive = True

    def create_session_and_cursor(self, cursor_config=None):
        uri = self.uri + self.file_name
        create_params = 'value_format={},key_format={}'.format(self.value_format, self.key_format)
        if self.use_colgroup:
            create_params += self.gen_colgroup_create_param()
        self.session.create(uri, create_params)

        # Add in column group.
        if self.use_colgroup:
            for i in range(0, len(self.value_format)):
                create_params = 'columns=(v{0}),'.format(i)
                suburi = 'colgroup:{0}:g{1}'.format(self.file_name, i)
                self.session.create(suburi, create_params)

        cursor = self.session.open_cursor(uri, None, cursor_config)
        self.session.begin_transaction()

        for i in range(self.start_key, self.end_key + 1):
            cursor[self.gen_key(i)] = self.gen_val("value" + str(i))
        self.session.commit_transaction()

        if (self.evict):
            evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
            for i in range(self.start_key, self.end_key):
                evict_cursor.set_key(self.gen_key(i))
                evict_cursor.search()
                evict_cursor.reset() 
        return cursor        

    def gen_colgroup_create_param(self):
        create_params = ",columns=("
        start = 0
        for _ in self.key_format:
            create_params += "k{0},".format(str(start)) 
            start += 1

        start = 0
        for _ in self.value_format:
            create_params += "v{0},".format(str(start)) 
            start += 1
        create_params += "),colgroups=("

        start = 0
        for _ in self.value_format:
            create_params += "g{0},".format(str(start)) 
            start += 1
        create_params += ")"
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

    def gen_val(self, i):
        tuple_val = []
        for key in self.value_format:
            if key == 'S' or key == 'u':
                tuple_val.append(str(i))
            elif key == "r":
                tuple_val.append(self.recno(i))
            elif key == "i":
                tuple_val.append(i)
        
        if (len(self.value_format) == 1):
            return tuple_val[0]
        else:
            return tuple(tuple_val)

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
                if (not self.upper_inclusive):
                    end_range -= 1

        if (lower_key):
            if (lower_key > start_range):
                start_range = lower_key
                if (not self.lower_inclusive):
                    start_range += 1


        count = ret = 0
        while True:
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
                self.assertTrue(self.check_key(lower_key) < key)
                
            if (self.upper_inclusive and upper_key):
                self.assertTrue(key <= self.check_key(upper_key))
            elif (upper_key):
                self.assertTrue(key < self.check_key(upper_key))
                
        if (expected_count != None):
            self.assertEqual(expected_count, count)
        else:
            self.assertEqual(end_range - start_range + 1, count)

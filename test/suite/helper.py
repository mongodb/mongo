#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
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

import os, string
import wiredtiger

# python has a filecmp.cmp function, but different versions of python approach
# file comparison differently.  To make sure we get byte for byte comparison,
# we define it here.
def compareFiles(self, filename1, filename2):
    self.pr('compareFiles: ' + filename1 + ', ' + filename2)
    bufsize = 4096
    if os.path.getsize(filename1) != os.path.getsize(filename2):
        print filename1 + ' size = ' + str(os.path.getsize(filename1))
        print filename2 + ' size = ' + str(os.path.getsize(filename2))
        return False
    with open(filename1, "rb") as fp1:
        with open(filename2, "rb") as fp2:
            while True:
                b1 = fp1.read(bufsize)
                b2 = fp2.read(bufsize)
                if b1 != b2:
                    return False
                # files are identical size
                if not b1:
                    return True

# confirm a URI doesn't exist.
def confirmDoesNotExist(self, uri):
    self.pr('confirmDoesNotExist: ' + uri)
    self.assertFalse(os.path.exists(uri))
    self.assertFalse(os.path.exists(uri + ".wt"))
    self.assertRaises(wiredtiger.WiredTigerError,
        lambda: self.session.open_cursor(uri, None, None))

# confirm a URI exists and is empty.
def confirmEmpty(self, uri):
    self.pr('confirmEmpty: ' + uri)
    cursor = self.session.open_cursor(uri, None, None)
    self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
    cursor.close()

# population of a simple object.
#    uri:       object
#    config:    session.create configuration string
#    rows:      entries to insert
def simplePopulate(self, uri, config, rows):
    self.pr('simplePopulate: ' + uri + ' with ' + str(rows) + ' rows')
    self.session.create(uri, config)
    cursor = self.session.open_cursor(uri, None, None)
    if string.find(cursor.key_format, "i") != -1:
        for i in range(0, rows):
            cursor.set_key(i)
            cursor.set_value(str(i) + ': abcdefghijklmnopqrstuvwxyz')
            cursor.insert()
    elif string.find(cursor.key_format, "S") != -1:
        for i in range(0, rows):
            cursor.set_key(str(i))
            cursor.set_value(str(i) + ': abcdefghijklmnopqrstuvwxyz')
            cursor.insert()
    else:
        raise AssertionError(
            'simplePopulate: configuration has unknown key type')
    cursor.close()

def simplePopulateCheck(self, uri):
    self.pr('simplePopulateCheck: ' + uri)
    cursor = self.session.open_cursor(uri, None, None)
    if string.find(cursor.key_format, "i") != -1:
	next = 0
        for key,val in cursor:
	    self.assertEqual(key, next)
	    self.assertEqual(val, str(next) + ': abcdefghijklmnopqrstuvwxyz')
	    next += 1
    elif string.find(cursor.key_format, "S") != -1:
	next = 0
        for key,val in cursor:
	    self.assertEqual(key, str(next))
	    self.assertEqual(val, str(next) + ': abcdefghijklmnopqrstuvwxyz')
	    next += 1
    else:
        raise AssertionError(
            'simplePopulateCheck: configuration has unknown key type')
    cursor.close()

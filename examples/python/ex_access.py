#!/usr/bin/env PYTHONPATH=../../lang/python:../../lang/python/src python
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
# ex_access.py
# 	demonstrates how to create and access a simple table.
#

import wiredtiger
import sys

home = 'WT_TEST'

try:
    conn = wiredtiger.wiredtiger_open(home, None, 'create')
    print('connected: ' + `conn`);
    session = conn.open_session(None, None)
except BaseException as e:
    print('Error connecting to', (home + ':'), e);
    sys.exit(1)

# Note: further error checking omitted for clarity.

session.create_table('access', 'key_format=S,value_format=S')
cursor = session.open_cursor('table:access', None, None)

# Insert a record.
cursor.set_key('key1')
cursor.set_value('value1')

# TODO: remove try block when cursor.insert works
try:
    cursor.insert()
except BaseException as tuple:
    print('Error cursor insert: ', tuple);
  
for key, value in cursor:
    print('Got record: ' + key + ' : ' + value)

conn.close(None)
sys.exit(0)

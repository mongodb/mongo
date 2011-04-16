#!/usr/bin/env PYTHONPATH=../../lang/python:../../lang/python/src python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
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

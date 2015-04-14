#!/usr/bin/python
# Public Domain 2014-2015 MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
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
# ex_stat.py
#      This is an example demonstrating how to query database statistics.

import os
from wiredtiger import wiredtiger_open,WIREDTIGER_VERSION_STRING
from _wiredtiger import WT_STAT_DSRC_BLOCK_SIZE,WT_STAT_DSRC_BLOCK_CHECKPOINT_SIZE,WT_STAT_DSRC_CURSOR_INSERT_BYTES,WT_STAT_DSRC_CURSOR_REMOVE_BYTES,WT_STAT_DSRC_CURSOR_UPDATE_BYTES,WT_STAT_DSRC_CACHE_BYTES_WRITE,WT_STAT_DSRC_BTREE_OVERFLOW

def main():
    # Create a clean test directory for this run of the test program
    os.system('rm -rf WT_HOME')
    os.makedirs('WT_HOME')
    # Connect to the database and open a session
    conn = wiredtiger_open('WT_HOME', 'create,statistics=(all)')
    session = conn.open_session()
    
    # Create a simple table
    session.create('table:access', 'key_format=S,value_format=S')
    
    # Open a cursor and insert a record
    cursor = session.open_cursor('table:access', None)

    cursor['key'] = 'value'    
    cursor.insert()
    cursor.close()
    
    session.checkpoint()
    print WIREDTIGER_VERSION_STRING
    print_database_stats(session)
    print_file_stats(session)
    print_overflow_pages(session)
    print_derived_stats(session)
    conn.close()

def print_database_stats(session):
    try:
        statcursor = session.open_cursor("statistics:")
    except:
        return
    print_cursor(statcursor)
    statcursor.close()

def print_file_stats(session):
    try:
        fstatcursor = session.open_cursor("statistics:table:access")
    except:
        return
    print_cursor(fstatcursor)
    fstatcursor.close()

def print_overflow_pages(session):
    try:
        ostatcursor = session.open_cursor("statistics:table:access")
    except:
        return
    val = ostatcursor[WT_STAT_DSRC_BTREE_OVERFLOW]
    if val != 0 :
        print str(val[0]) + '=' + str(val[1])
    ostatcursor.close()

def print_derived_stats(session):
    try:
        dstatcursor = session.open_cursor("statistics:table:access")
    except:
        return
    ckpt_size = dstatcursor[WT_STAT_DSRC_BLOCK_CHECKPOINT_SIZE][1]
    file_size = dstatcursor[WT_STAT_DSRC_BLOCK_SIZE][1]
    percent = 0
    if file_size != 0 :
        percent = 100 * ((float(file_size) - float(ckpt_size)) / float(file_size))
    print "Table is %" + str(percent) + " fragmented"

    app_insert = int(dstatcursor[WT_STAT_DSRC_CURSOR_INSERT_BYTES][1])
    app_remove = int(dstatcursor[WT_STAT_DSRC_CURSOR_REMOVE_BYTES][1])
    app_update = int(dstatcursor[WT_STAT_DSRC_CURSOR_UPDATE_BYTES][1])
    fs_writes  = int(dstatcursor[WT_STAT_DSRC_CACHE_BYTES_WRITE][1])

    if(app_insert + app_remove + app_update != 0):
        print "Write amplification is " + '{:.2f}'.format(fs_writes / (app_insert + app_remove + app_update))
    dstatcursor.close()


def print_cursor(mycursor):
    while mycursor.next() == 0:
        val = mycursor.get_value()
        if val[1] != '0' :
            print str(val[0]) + '=' + str(val[1])

if __name__ == "__main__":
    main()


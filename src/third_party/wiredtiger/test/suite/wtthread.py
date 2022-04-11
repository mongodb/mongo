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

import os, queue, shutil, threading, time, wiredtiger, wttest
from helper import compare_tables

class checkpoint_thread(threading.Thread):
    def __init__(self, conn, done):
        self.conn = conn
        self.done = done
        threading.Thread.__init__(self)

    def run(self):
        sess = self.conn.open_session()
        while not self.done.isSet():
            # Sleep for 10 milliseconds.
            time.sleep(0.001)
            sess.checkpoint()
        sess.close()

class flush_tier_thread(threading.Thread):
    def __init__(self, conn, done):
        self.conn = conn
        self.done = done
        threading.Thread.__init__(self)

    def run(self):
        sess = self.conn.open_session()
        while not self.done.isSet():
            # Sleep for 25 milliseconds.
            time.sleep(0.0025)
            sess.flush_tier()
        sess.close()

class backup_thread(threading.Thread):
    def __init__(self, conn, backup_dir, done):
        self.backup_dir = backup_dir
        self.conn = conn
        self.done = done
        threading.Thread.__init__(self)

    def run(self):
        sess = self.conn.open_session()
        while not self.done.isSet():
            # Sleep for 2 seconds.
            time.sleep(2)
            sess.checkpoint()
            shutil.rmtree(self.backup_dir, ignore_errors=True)
            os.mkdir(self.backup_dir)
            cursor = sess.open_cursor('backup:', None, None)
            files = list()
            while True:
                ret = cursor.next()
                if ret != 0:
                    break
                files.append(cursor.get_key())
                shutil.copy(cursor.get_key(), self.backup_dir)

            cursor.close()

            bkp_conn = None
            try:
                bkp_conn = wiredtiger.wiredtiger_open(self.backup_dir)
                bkp_session = bkp_conn.open_session()
                # Verify that the backup was OK.
                uris = list()
                for next_file in files:
                    if next_file.startswith("WiredTiger"):
                        continue
                    uri = "file:" + next_file
                    uris.append(uri)

                # Add an assert to stop running the test if any difference in table contents
                # is found. We would have liked to use self.assertTrue instead, but are unable
                # to because backup_thread does not support this method unless it is a wttest.
                wttest.WiredTigerTestCase.printVerbose(3, "Testing if checkpoint tables match:")
                assert compare_tables(self, sess, uris) == True
                wttest.WiredTigerTestCase.printVerbose(3, "Checkpoint tables match")

                wttest.WiredTigerTestCase.printVerbose(3, "Testing if backup tables match:")
                assert compare_tables(self, bkp_session, uris) == True
                wttest.WiredTigerTestCase.printVerbose(3, "Backup tables match")
            finally:
                if bkp_conn != None:
                    bkp_conn.close()

        sess.close()

# A worker thread that pulls jobs off a queue. The jobs can be different types.
# Currently supported types are:
# 'i' for insert.
# 'b' for bounce (close and open) a session handle
# 'd' for drop a table
# 't' for create a table and insert a single item into it
class op_thread(threading.Thread):
    def __init__(self, conn, uris, key_fmt, work_queue, done):
        self.conn = conn
        self.uris = uris
        self.key_fmt = key_fmt
        self.work_queue = work_queue
        self.done = done
        threading.Thread.__init__(self)

    def run(self):
        sess = self.conn.open_session()
        if (len(self.uris) == 1):
            c = sess.open_cursor(self.uris[0], None, None)
        else:
            cursors = list()
            for next_uri in self.uris:
                cursors.append(sess.open_cursor(next_uri, None, None))
        while not self.done.isSet():
            try:
                op, key, value = self.work_queue.get_nowait()
                if op == 'gi': # Group insert a number of tables.
                    sess.begin_transaction()
                    for next_cur in cursors:
                        next_cur[key] = value
                    sess.commit_transaction()
                if op == 'gu': # Group update a number of tables.
                    sess.begin_transaction()
                    for next_cur in cursors:
                        next_cur.set_key(key)
                        next_cur.set_value(value)
                        next_cur.update()
                        next_cur.reset()
                    sess.commit_transaction()
                if op == 'i': # Insert an item
                    c[key] = value
                elif op == 'b': # Bounce the session handle.
                    c.close()
                    sess.close()
                    sess = self.conn.open_session()
                    c = sess.open_cursor(self.uris[0], None, None)
                elif op == 'd': # Drop a table. The uri identifier is in key
                    for i in range(10):
                        try:
                            sess.drop(self.uris[0] + str(key))
                            break
                        except wiredtiger.WiredTigerError as e:
                            continue
                elif op == 't': # Create a table, add an entry
                    sess.create(self.uris[0] + str(key),
                        "key_format=" + self.key_fmt + ",value_format=S")
                    try:
                        c2 = sess.open_cursor(self.uris[0] + str(key), None, None)
                        c2[key] = value
                        c2.close()
                    except wiredtiger.WiredTigerError as e:
                        # These operations can fail, if the drop in another
                        # thread happened
                        pass
                self.work_queue.task_done()
            except queue.Empty:
                # Wait on the queue until done is flagged
                time.sleep(0.01)
        if (len(self.uris) == 1):
            c.close()
        else:
            for next_cursor in cursors:
                next_cursor.close()
        sess.close()

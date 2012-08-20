#!/usr/bin/env python
#
# Public Domain 2008-2012 WiredTiger, Inc.
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

import Queue
import threading ,time, wiredtiger, wttest
from helper import key_populate, simple_populate

# test_checkpoint02.py
#   Run background checkpoints repeatedly while doing inserts and other
#   operations in another thread

done = False

class CheckpointThread(threading.Thread):
    def __init__(self, conn):
        self.conn = conn
        threading.Thread.__init__(self)
 
    def run(self):
        global done
        sess = self.conn.open_session()
        while not done:
            # Sleep for 10 milliseconds.
            time.sleep(0.001)
            sess.checkpoint()
        sess.close()

# A worked thread that pulls jobs off a queue. The jobs can be different types.
# Currently supported types are:
# 'i' for insert.
# 'b' for bounce (close and open) a session handle
# 'd' for drop a table
# 't' for create a table and insert a single item into it
class OpThread(threading.Thread):
    def __init__(self, conn, uri, key_fmt, queue):
        self.conn = conn
        self.uri = uri
        self.key_fmt = key_fmt
        self.queue = queue
        threading.Thread.__init__(self)
 
    def run(self):
        global done
        sess = self.conn.open_session()
        c = sess.open_cursor(self.uri, None, None)
        while not done:
            try:
                op, key, value = self.queue.get_nowait()
                if op == 'i': # Insert an item
                    c.set_key(key)
                    c.set_value(value)
                    c.insert()
                elif op == 'b': # Bounce the session handle.
                    c.close()
                    sess.close()
                    sess = self.conn.open_session()
                    c = sess.open_cursor(self.uri, None, None)
                elif op == 'd': # Drop a table. The uri identifier is in key
                    for i in range(10):
                        try:
                            sess.drop(self.uri + str(key))
                            break
                        except wiredtiger.WiredTigerError as e:
                            continue
                elif op == 't': # Create a table, add an entry
                    sess.create(self.uri + str(key),
                        "key_format=" + self.key_fmt + ",value_format=S")
                    try:
                        c2 = sess.open_cursor(self.uri + str(key), None, None)
                        c2.set_key(key)
                        c2.set_value(value)
                        c2.insert()
                        c2.close()
                    except wiredtiger.WiredTigerError as e:
                        # These operations can fail, if the drop in another
                        # thread happened
                        pass
                self.queue.task_done()
            except Queue.Empty:
                # Wait on the queue until done is flagged
                time.sleep(0.01)
        c.close()
        sess.close()

class test_checkpoint(wttest.WiredTigerTestCase):
    scenarios = [
        ('table', dict(uri='table:test',fmt='L',dsize=100,nops=50000,nthreads=10)),
        ('table', dict(uri='table:test',fmt='L',dsize=10,nops=50000,nthreads=30))
        ]


    def test_checkpoint(self):
        global done
        done = False
        self.session.create(self.uri,
            "key_format=" + self.fmt + ",value_format=S")
        ckpt = CheckpointThread(self.conn)
        ckpt.start()

        queue = Queue.Queue()
        my_data = 'a' * self.dsize
        for i in xrange(self.nops):
            if i % 191 == 0 and i != 0:
                queue.put_nowait(('b', i, my_data))
            if i % 257 == 0 and i != 0:
                queue.put_nowait(('t', i, my_data))
            # Wait another 200 operations, then delete the above table. This
            # not guarantee that the initial operations on the table will have
            # been finished.
            if (i - 100) % 257 == 0 and (i - 100) != 0:
                queue.put_nowait(('d', i - 100, my_data))
            queue.put_nowait(('i', i, my_data))

        for i in xrange(self.nthreads):
            t = OpThread(self.conn, self.uri, self.fmt, queue)
            t.start()

        queue.join()
        done = True
        # Wait for checkpoint thread to notice status change.
        while ckpt.is_alive():
            time.sleep(0.01)

        # Create a cursor - ensure all items have been put.
        cursor = self.session.open_cursor(self.uri, None, None)
        i = 0
        while True:
            nextret = cursor.next()
            if nextret != 0:
                break
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, i)
            self.assertEqual(value, my_data)
            i += 1

        self.assertEqual(i, self.nops)


if __name__ == '__main__':
    wttest.run()

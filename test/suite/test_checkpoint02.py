#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import Queue
import threading, time, wiredtiger, wttest
from helper import key_populate, simple_populate
from wtthread import checkpoint_thread, op_thread
from wtscenario import check_scenarios

# test_checkpoint02.py
#   Run background checkpoints repeatedly while doing inserts and other
#   operations in another thread
class test_checkpoint02(wttest.WiredTigerTestCase):
    scenarios = check_scenarios([
        ('table-100', dict(uri='table:test',fmt='L',dsize=100,nops=50000,nthreads=10)),
        ('table-10', dict(uri='table:test',fmt='L',dsize=10,nops=50000,nthreads=30))
    ])

    def test_checkpoint02(self):
        done = threading.Event()
        self.session.create(self.uri,
            "key_format=" + self.fmt + ",value_format=S")
        ckpt = checkpoint_thread(self.conn, done)
        ckpt.start()

        uris = list()
        uris.append(self.uri)
        queue = Queue.Queue()
        my_data = 'a' * self.dsize
        for i in xrange(self.nops):
            if i % 191 == 0 and i != 0:
                queue.put_nowait(('b', i, my_data))
            queue.put_nowait(('i', i, my_data))

        opthreads = []
        for i in xrange(self.nthreads):
            t = op_thread(self.conn, uris, self.fmt, queue, done)
            opthreads.append(t)
            t.start()

        queue.join()
        done.set()
        for t in opthreads:
            t.join()
        ckpt.join()

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

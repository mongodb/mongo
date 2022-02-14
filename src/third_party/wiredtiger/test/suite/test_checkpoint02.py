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
#
# [TEST_TAGS]
# checkpoint
# [END_TAGS]
#

import queue, threading, wttest
from wtthread import checkpoint_thread, op_thread
from wtscenario import make_scenarios

# test_checkpoint02.py
#   Run background checkpoints repeatedly while doing inserts and other
#   operations in another thread
class test_checkpoint02(wttest.WiredTigerTestCase):
    format_values = [
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='S')),
        ('u32_row', dict(key_format='L', value_format='S')),
    ]

    size_values = [
        ('table-100', dict(uri='table:test',dsize=100,nops=50000,nthreads=10)),
        ('table-10', dict(uri='table:test',dsize=10,nops=50000,nthreads=30))
    ]

    scenarios = make_scenarios(format_values, size_values)

    def test_checkpoint02(self):
        done = threading.Event()
        self.session.create(self.uri,
            "key_format={},value_format={}".format(self.key_format, self.value_format))

        if self.value_format == '8t':
            self.nops *= 2
            my_data = 97
        else:
            my_data = 'a' * self.dsize

        ckpt = checkpoint_thread(self.conn, done)
        work_queue = queue.Queue()
        opthreads = []
        try:
            ckpt.start()

            uris = list()
            uris.append(self.uri)
            for i in range(self.nops):
                if i % 191 == 0 and i != 0:
                    work_queue.put_nowait(('b', i + 1, my_data))
                work_queue.put_nowait(('i', i + 1, my_data))

            for i in range(self.nthreads):
                t = op_thread(self.conn, uris, self.key_format, work_queue, done)
                opthreads.append(t)
                t.start()
        except:
            # Deplete the work queue if there's an error.
            while not work_queue.empty():
                work_queue.get()
                work_queue.task_done()
            raise
        finally:
            work_queue.join()
            done.set()
            for t in opthreads:
                t.join()
            ckpt.join()

        # Create a cursor - ensure all items have been put.
        cursor = self.session.open_cursor(self.uri, None, None)
        i = 1
        while True:
            nextret = cursor.next()
            if nextret != 0:
                break
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, i)
            self.assertEqual(value, my_data)
            i += 1

        self.assertEqual(i, self.nops + 1)

if __name__ == '__main__':
    wttest.run()

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

import queue, threading, time, wttest
from wtthread import backup_thread, op_thread

# test_backup02.py
#   Run background checkpoints and backups repeatedly while doing inserts
#   in another thread
class test_backup02(wttest.WiredTigerTestCase):
    uri = 'table:test_backup02'
    fmt = 'L'
    dsize = 100
    nops = 200
    nthreads = 1
    time = 60 if wttest.islongtest() else 10

    def test_backup02(self):
        done = threading.Event()
        uris = list()
        uris.append(self.uri + str(1))
        uris.append(self.uri + str(2))
        uris.append(self.uri + str(3))
        for this_uri in uris:
            self.session.create(this_uri,
                "key_format=" + self.fmt + ",value_format=S")
        # TODO: Ideally we'd like a checkpoint thread, separate to the backup
        # thread. Need a way to stop checkpoints while doing backups.
#        ckpt = checkpoint_thread(self.conn, done)
#        ckpt.start()
        bkp = backup_thread(self.conn, 'backup.dir', done)
        work_queue = queue.Queue()
        opthreads = []
        try:
            bkp.start()

            my_data = 'a' * self.dsize
            for i in range(self.nops):
                work_queue.put_nowait(('gi', i, my_data))

            for i in range(self.nthreads):
                t = op_thread(self.conn, uris, self.fmt, work_queue, done)
                opthreads.append(t)
                t.start()

            # Add 200 update entries into the queue every .1 seconds.
            more_time = self.time
            while more_time > 0:
                time.sleep(0.1)
                my_data = str(more_time) + 'a' * (self.dsize - len(str(more_time)))
                more_time = more_time - 0.1
                for i in range(self.nops):
                    work_queue.put_nowait(('gu', i, my_data))
        except:
            # Deplete the work queue if there's an error.
            while not work_queue.empty():
                work_queue.get()
                work_queue.task_done()
            raise
        finally:
            work_queue.join()
            done.set()
            # Wait for checkpoint thread to notice status change.
            # ckpt.join()
            for t in opthreads:
                t.join()
            bkp.join()

if __name__ == '__main__':
    wttest.run()

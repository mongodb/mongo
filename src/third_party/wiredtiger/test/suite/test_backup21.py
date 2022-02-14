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

import queue, threading, wttest
from wtbackup import backup_base
from wtthread import op_thread

# test_backup21.py
#    Run create/drop operations while backup is ongoing.
class test_backup21(backup_base):
    # Backup directory name.
    dir = 'backup.dir'
    uri = 'test_backup21'
    ops = 50
    key_fmt = "S"

    def test_concurrent_operations_with_backup(self):
        done = threading.Event()
        table_uri = 'table:' + self.uri

        # Create and populate the table.
        self.session.create(table_uri, "key_format=S,value_format=S")
        self.add_data(table_uri, 'key', 'value', True)

        work_queue = queue.Queue()
        t = op_thread(self.conn, [table_uri], self.key_fmt, work_queue, done)
        try:
            t.start()
            # Place create or drop operation into work queue.
            iteration = 0
            op = 't'
            for _ in range(0, self.ops):
                # Open backup cursor.
                bkup_c = self.session.open_cursor('backup:', None, None)
                work_queue.put_nowait((op, str(iteration), 'value'))

                all_files = self.take_full_backup(self.dir, bkup_c)
                if op == 't':
                    # Newly created table shouldn't be present in backup.
                    self.assertTrue(self.uri + str(iteration) + ".wt" not in all_files)
                    iteration = iteration + 1
                else:
                    # Dropped table should still be present in backup.
                    self.assertTrue(self.uri + str(iteration) + ".wt" in all_files)
                    iteration = iteration + 1
                bkup_c.close()
                # Once we reach midway point, start drop operations.
                if iteration == self.ops/2:
                    iteration = 0
                    op = 'd'
        except:
            # Deplete the work queue if there's an error.
            while not work_queue.empty():
                work_queue.get()
                work_queue.task_done()
            raise
        finally:
            work_queue.join()
            done.set()
            t.join()

if __name__ == '__main__':
    wttest.run()

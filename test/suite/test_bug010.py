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
#
# test_bug010.py
#       check that checkpoints don't leave files marked clean when they
#       did not write all updates out.
#

import wiredtiger, wttest, wtthread
import threading, time

class test_bug010(wttest.WiredTigerTestCase):
    name = 'test_bug010'
    uri = 'table:' + name
    num_tables = 1000

    # Disable checkpoint sync, to make checkpoints faster and
    # increase the likelyhood of triggering the symptom
    conn_config = 'checkpoint_sync=false'

    def test_checkpoint_dirty(self):
        # Create a lot of tables
        # insert the same item in each
        # Start a checkpoint with some of the updates
        # Create another checkpoint that should contain all data consistently
        # Read from the checkpoint and make sure the data is consistent
        for i in range(0, self.num_tables):
            self.printVerbose(3, 'Creating table ' + str(i))
            self.session.create(self.uri + str(i),
            'key_format=S,value_format=i')
            c = self.session.open_cursor(self.uri + str(i), None)
            c['a'] = 0
            c.close()

        self.session.checkpoint()

        iterations = 1
        expected_val = 0
        for its in range(1, 10):
            self.printVerbose(3, 'Doing iteration ' + str(its))

            # Create a checkpoint thread
            done = threading.Event()
            ckpt = wtthread.checkpoint_thread(self.conn, done)
            ckpt.start()
            try:
                expected_val += 1
                for i in range(0, self.num_tables):
                    c = self.session.open_cursor(self.uri + str(i), None)
                    c['a'] = expected_val
                    c.close()
            finally:
                done.set()
                ckpt.join()

            # Execute another checkpoint, to make sure we have a consistent
            # view of the data.
            self.session.checkpoint()
            for i in range(0, self.num_tables):
                c = self.session.open_cursor(
                    self.uri + str(i), None, 'checkpoint=WiredTigerCheckpoint')
                c.next()
                self.assertEquals(c.get_value(), expected_val,
                    msg='Mismatch on iteration ' + str(its) +\
                                        ' for table ' + str(i))
                c.close()

if __name__ == '__main__':
    wttest.run()

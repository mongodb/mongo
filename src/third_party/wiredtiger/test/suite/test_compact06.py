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

import wiredtiger, wttest
from wtscenario import make_scenarios
from wtdataset import SimpleDataSet

# test_compact06.py
# Test background compaction API usage.
class test_compact06(wttest.WiredTigerTestCase):
    uri = 'file:test_compact06'
    
    def test_background_compact_api(self):
        # Create a table.
        self.session.create(self.uri, 'key_format=i,value_format=S')
        
        # Test for invalid uses of the compact API:
        #   1. We cannot trigger the background compaction on a specific API.
        with self.expectedStderrPattern(
            'Background compaction does not work on specific URIs.'):
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                self.session.compact(self.uri, 'background=true'))        
            
        #   2. We cannot set other configurations while turning off the background server.
        with self.expectedStderrPattern(
            'free_space_target configuration cannot be set when disabling the background compaction server.'):
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                self.session.compact(None, 'background=false,free_space_target=10MB'))
        with self.expectedStderrPattern(
            'timeout configuration cannot be set when disabling the background compaction server.'):
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                self.session.compact(None, 'background=false,timeout=60'))
            
        #   3. We cannot reconfigure the background server.
        # FIXME: WT-11421 Enable once fix handles ret value being overridden in background compact.
        # self.session.compact(None, 'background=true')
        # with self.expectedStderrPattern('Background compaction is already'):
        #     self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
        #         self.session.compact(None, 'background=true,free_space_target=10MB'))

if __name__ == '__main__':
    wttest.run()

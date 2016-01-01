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

import wiredtiger, wtscenario, wttest

# test_unicode01.py
#   Make sure UTF8 config can be passed to WT_SESSION::create
#
#   There are a couple of tricks here:
#   1) we don't want to treat the whole file as UTF-8, because this would
#      be the only one in our tree and that causes problems for some
#      scripts; and
#   2) we can't pass in a Unicode object directly because the
#      SWIG-generated code expects a simple Python string.
class test_unicode01(wttest.WiredTigerTestCase):
    def test_unicode(self):
        self.session.create('table:t',
            u'app_metadata={"name" : "Employ\xe9s"}'.encode('utf-8'))

if __name__ == '__main__':
    wttest.run()

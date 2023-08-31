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

import os, threading, unittest

# Custom result class that will prefix the pid in text output (including if it's a child).
# Only enabled when we are in verbose mode so we don't check that here.
class PidAwareTextTestResult(unittest.TextTestResult):
    _thread_prefix = threading.local()

    def __init__(self, stream, descriptions, verbosity):
        super(PidAwareTextTestResult, self).__init__(stream, descriptions, verbosity)
        self._thread_prefix.value = "[pid:{}]: ".format(os.getpid())

    def tags(self, new_tags, gone_tags):
        # We attach the PID to the thread so we only need the new_tags.
        for tag in new_tags:
            if tag.startswith("pid:"):
                pid = tag[len("pid:"):]
                self._thread_prefix.value = "[pid:{}/{}]: ".format(os.getpid(), pid)

    def startTest(self, test):
        self.stream.write(self._thread_prefix.value)
        super(PidAwareTextTestResult, self).startTest(test)

    def getDescription(self, test):
        return str(test.shortDescription())

    def printErrorList(self, flavour, errors):
        for test, err in errors:
            self.stream.writeln(self.separator1)
            self.stream.writeln("%s%s: %s" % (self._thread_prefix.value,
                flavour, self.getDescription(test)))
            self.stream.writeln(self.separator2)
            self.stream.writeln("%s%s" % (self._thread_prefix.value, err))
            self.stream.flush()

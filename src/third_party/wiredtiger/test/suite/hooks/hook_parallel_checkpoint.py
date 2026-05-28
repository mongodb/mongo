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
# ignored_file
# [END_TAGS]

# hook_parallel_checkpoint.py
#
# Enable parallel checkpoints for all Python unit tests by appending a
# checkpoint_threads=... configuration to wiredtiger_open().
#
# Usage examples:
#   ../test/suite/run.py --hook parallel_checkpoint
#   ../test/suite/run.py --hook parallel_checkpoint=8
#   ../test/suite/run.py --hook "parallel_checkpoint=(threads=8)"
#
# The hook will not override tests with explicit checkpoint_threads= in the
# connection configuration string.

from __future__ import print_function

import re, wthooks

def _parse_threads(arg):
    # Default to 4 threads if no argument is provided
    if not arg:
        return 4

    s = str(arg).strip()
    if s.startswith('(') and s.endswith(')'):
        s = s[1:-1]

    threads = None
    if re.fullmatch(r'[0-9]+', s):
        threads = int(s)
    else:
        m = re.fullmatch(r'threads=([0-9]+)', s)
        if m:
            threads = int(m.group(1))
    if threads is None:
        raise Exception(
            'hook_parallel_checkpoint: invalid argument "{}"'.format(arg)
        )
    if threads <= 0:
        raise Exception(
            'hook_parallel_checkpoint: threads must be > 0, got {}'.format(threads)
        )
    return threads

class ParallelCheckpointHookCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, arg=None):
        self.threads = _parse_threads(arg)
        self.platform_api = wthooks.DefaultPlatformAPI()

    def get_platform_api(self):
        return self.platform_api

    def register_skipped_tests(self, tests):
        pass

    def setup_hooks(self):
        threads = self.threads

        def wiredtiger_open_args(ignored_self, args):
            args = list(args)
            config = args[1] if len(args) >= 2 else ''
            if config is None:
                config = ''
            if 'checkpoint_threads=' in config:
                return args
            if len(args) >= 2:
                args[1] = config + ',checkpoint_threads={}'.format(threads)
            else:
                args.append(',checkpoint_threads={}'.format(threads))
            return args

        self.wiredtiger['wiredtiger_open'] = (wthooks.HOOK_ARGS, wiredtiger_open_args)

def initialize(arg):
    return [ParallelCheckpointHookCreator(arg)]

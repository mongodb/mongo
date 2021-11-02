#!/usr/bin/python
# -*- coding: utf-8 -*-

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

class WTPerfConfig:
    def __init__(self,
                 wtperf_path: str,
                 home_dir: str,
                 test: str,
                 arg_file: str = None,
                 environment: str = None,
                 run_max: int = 1,
                 verbose: bool = False,
                 git_root: str = None,
                 json_info: dict = {}):
        self.wtperf_path: str = wtperf_path
        self.home_dir: str = home_dir
        self.test: str = test
        self.arg_file = arg_file
        self.environment: str = environment
        self.run_max: int = run_max
        self.verbose: bool = verbose
        self.git_root: str = git_root
        self.json_info: dict = json_info

    def to_value_dict(self):
        as_dict = {'wt_perf_path': self.wtperf_path,
                   'test': self.test,
                   'arg_file': self.arg_file,
                   'home_dir': self.home_dir,
                   'environment': self.environment,
                   'run_max': self.run_max,
                   'verbose': self.verbose,
                   'git_root': self.git_root,
                   'json_info': self.json_info}
        return as_dict

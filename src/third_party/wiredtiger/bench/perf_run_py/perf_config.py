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

class TestType:
    def __init__(self, is_wtperf: bool, is_workgen: bool):
        self.is_wtperf = is_wtperf
        self.is_workgen = is_workgen

    def get_home_arg(self, home: str):
        if self.is_wtperf:
            return ['-h', home]
        if self.is_workgen:
            return ['--home', home]

    def get_test_arg(self, test: str):
        if self.is_wtperf:
            return ['-O', test]
        if self.is_workgen:
            return [test]


class PerfConfig:
    def __init__(self,
                 test_type: TestType,
                 exec_path: str,
                 home_dir: str,
                 test: str,
                 batch_file: str = None,
                 arguments=None,
                 operations=None,
                 run_max: int = 1,
                 verbose: bool = False,
                 git_root: str = None,
                 json_info=None):
        if json_info is None:
            json_info = {}
        self.test_type: TestType = test_type
        self.exec_path: str = exec_path
        self.home_dir: str = home_dir
        self.test: str = test
        self.batch_file = batch_file
        self.arguments = arguments
        self.operations = operations
        self.run_max: int = run_max
        self.verbose: bool = verbose
        self.git_root: str = git_root
        self.json_info: dict = json_info

    def to_value_dict(self):
        as_dict = {'exec_path': self.exec_path,
                   'test': self.test,
                   'batch_file': self.batch_file,
                   'arguments': self.arguments,
                   'operations': self.operations,
                   'home_dir': self.home_dir,
                   'run_max': self.run_max,
                   'verbose': self.verbose,
                   'git_root': self.git_root,
                   'json_info': self.json_info}
        return as_dict

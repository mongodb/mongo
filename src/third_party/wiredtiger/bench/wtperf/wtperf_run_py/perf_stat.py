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

class PerfStat:
    def __init__(self,
                 short_label: str,
                 pattern: str,
                 input_offset: int,
                 output_label: str,
                 output_precision: int = 0,
                 conversion_function=int):
        self.short_label: str = short_label
        self.pattern: str = pattern
        self.input_offset: int = input_offset
        self.output_label: str = output_label
        self.output_precision: int = output_precision
        self.conversion_function = conversion_function
        self.values = []

    def add_value(self, value):
        converted_value = self.conversion_function(value)
        self.values.append(converted_value)

    def get_num_values(self):
        return len(self.values)

    def get_average(self):
        num_values = len(self.values)
        total = sum(self.values)
        average = self.conversion_function(total / num_values)
        return average

    def get_skipminmax_average(self):
        num_values = len(self.values)
        assert num_values >= 3
        minimum = min(self.values)
        maximum = max(self.values)
        total = sum(self.values)
        total_skipminmax = total - maximum - minimum
        num_values_skipminmax = num_values - 2
        skipminmax_average = self.conversion_function(total_skipminmax / num_values_skipminmax)
        return skipminmax_average

    def get_core_average(self):
        if len(self.values) >= 3:
            return self.get_skipminmax_average()
        else:
            return self.get_average()

    def are_values_all_zero(self):
        result = True
        for value in self.values:
            if value != 0:
                result = False
        return result

#!/usr/bin/env python3
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

# When calling '__wt_verbose', the second parameter can only be a single verbose
# category definition i.e. can't treat the parameter as a flag/mask value.
# Iterate over uses of '__wt_verbose' and detect any invalid uses where multiple
# verbose categories are bitwise OR'd.
import re, sys

verbose_regex = re.compile('([0-9]+):\\s*__wt_verbose\\(.*?,(.*?)[\\"\\\']')
bitwise_or_regex = re.compile(r'^.*(?<!\|)\|(?!\|).*$')
for line in sys.stdin:
    # Find all uses of __wt_verbose in a given line, capturing the line number
    # and 2nd paramter as groups.
    m = verbose_regex.findall(line)
    if len(m) != 0:
        for verb_match in m:
            if len(verb_match) != 2:
                continue
            line = verb_match[0]
            verb_parameter = verb_match[1]
            # Test if the verbose category parameter uses a bitwise OR.
            bit_m = bitwise_or_regex.search(verb_parameter)
            if bit_m != None:
                sys.stdout.write(line)
                sys.stdout.write('\n')

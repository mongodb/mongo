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

import os, re
from dist import compare_srcfile

verbose_categories = []
filename = '../src/include/wiredtiger.h.in'
start_tag = 'VERBOSE ENUM START'
stop_tag = 'VERBOSE ENUM STOP'

# Retrieve all verbose categories.
with open(filename, 'r') as f:
    in_section = False
    pattern = re.compile("^WT_VERB_[A-Z_]+$")
    for line in f:
        if line.find(start_tag) != -1:
            in_section = True
            continue
        if line.find(stop_tag) != -1:
            break
        if in_section:
            # Remove any leading and trailing whitespaces.
            line = line.strip()
            content = line.split(',')

            # The length will be <= 1 if no comma is present.
            assert len(content) > 1, content
            verbose_category = content[0]
            # Check the value follows the expected format.
            assert pattern.match(verbose_category), \
                   'The category '  + verbose_category + ' does not follow the expected syntax.'

            # Save the category.
            verbose_categories.append(content[0])

    assert len(verbose_categories) > 0, 'No verbose categories have been found in ' + filename
f.close()

filename = '../src/include/verbose.h'
start_tag = 'AUTOMATIC VERBOSE ENUM STRING GENERATION START'
stop_tag = 'AUTOMATIC VERBOSE ENUM STRING GENERATION STOP'

# Generate all verbose categories as strings
tmp_file = '__tmp_verbose' + str(os.getpid())
with open(filename, 'r') as f:

    tfile = open(tmp_file, 'w')
    end_found = False
    in_section = False
    start_found = False

    for line in f:

        line_tmp = line

        if line.find(stop_tag) != -1:
            assert start_found, 'Missing start tag: ' + start_tag
            assert not end_found, 'Found end tag again: ' + stop_tag

            end_found = True
            in_section = False

        elif line.find(start_tag) != -1:
            assert not start_found and not end_found, 'Duplicate tag'

            start_found = True
            in_section = True

            escape_char = ''
            if line.strip().endswith('\\'):
                escape_char = '\\'

            indentation = len(line) - len(line.lstrip())

            for category in verbose_categories:
                line_tmp += indentation * ' ' + "\"" + category + "\", %s\n" % escape_char

        elif in_section:
            continue

        tfile.write(line_tmp)

    assert not in_section and start_found and end_found, 'File syntax incorrect'

    tfile.close()
    compare_srcfile(tmp_file, filename)

f.close()

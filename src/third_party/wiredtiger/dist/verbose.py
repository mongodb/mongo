#!/usr/bin/env python

import re
from dist import compare_srcfile

verbose_categories = []
filename = '../src/include/wiredtiger.in'
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
tmp_file = '__tmp_verbose'
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

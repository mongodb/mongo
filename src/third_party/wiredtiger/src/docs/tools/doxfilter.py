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


# This Python script is run as part of generating the documentation for the
# WiredTiger reference manual.  It changes comments to Javadoc style
# (i.e., from "/*!" to "/**"), because the latter are configured to not
# search for brief descriptions at the beginning of pages.

import os, re, sys

# We want to import the docs_data.py page from the dist directory.
# First get our (src/doc/tools) directory.
doc_tools_dir = os.path.dirname(os.path.realpath(__file__))
top_dir = os.path.dirname(os.path.dirname(os.path.dirname(doc_tools_dir)))
dist_dir = os.path.join(top_dir, 'dist')
sys.path.insert(1, dist_dir)
import docs_data

arch_doc_lookup = {}
for page in docs_data.arch_doc_pages:
    arch_doc_lookup[page.doxygen_name] = page

progname = 'doxfilter.py'
linenum = 0
filename = '<unknown>'

def err(arg):
    sys.stderr.write(filename + ':' + str(linenum) + ': ERROR: ' + arg + '\n')
    sys.exit(1)

# Convert @arch_page to @arch_page_expanded, adding in information
# from docs_data.py.
def process_arch(source):
    result = ''
    mpage_content = []
    arch_page_pat = re.compile(r'^(.*)@arch_page  *([^ ]*)  *(.*)')
    for line in source.split('\n'):
        m = re.search(arch_page_pat, line)
        if line.count('@arch_page') > 0 and not m:
            err('@arch_page incorrect syntax, need identifier and title')
        if m:
            groups = m.groups()
            prefix = groups[0]
            doxy_name = groups[1]
            title = groups[2]

            page_info = arch_doc_lookup[doxy_name]
            data_structures_str = '<code>' + '<br>'.join(page_info.data_structures) + '</code>'
            files_str = '<code>' + '<br>'.join(page_info.files) + '</code>'
            result += prefix + '@arch_page_top{' + \
                doxy_name + ',' + \
                title + '}\n'
            result += '@arch_page_table{' + \
                data_structures_str + ',' + \
                files_str + '}\n'
        else:
            result += line + '\n'
    return result

def process(source):
    source = source.replace(r'/*!', r'/**')
    if '@arch_page' in source:
        source = process_arch(source)
    return source

if __name__ == '__main__':
    for f in sys.argv[1:]:
        filename = f
        with open(f, 'r') as infile:
            sys.stdout.write(process(infile.read()))
        sys.exit(0)

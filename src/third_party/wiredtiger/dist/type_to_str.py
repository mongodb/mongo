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

from dist import compare_srcfile, format_srcfile

"""
* docstring: The documentation to attach to the generated function.
* name: The name to use for the generated function, i.e. `__wt_name_str`.
* define_regex: The regular expression to use for capturing the names of the
  macros. Note that this must also include the capture group.
* srcfile: The source file to look through when searching for `define_regex`.
* out: The output file handle.
"""
def generate_string_conversion(docstring, name, define_regex, srcfile, out):
    srcfile = '../' + srcfile
    pattern = re.compile(define_regex)

    out.write('/*\n')
    out.write(' * __wt_{}_str --\n'.format(name))
    out.write(' *     {}\n'.format(docstring))
    out.write(' */\n')
    out.write('static WT_INLINE const char *\n')
    out.write('__wt_{}_str(uint8_t val)\n'.format(name))
    out.write('{\n')
    out.write('    switch (val) {\n')

    found = False
    with open(srcfile, 'r') as src:
        for line in src:
            matches = pattern.search(line)
            if matches:
                found = True
                out.write('    case {}:\n'.format(matches.group(1)))
                out.write('        return ("{}");\n'.format(matches.group(1)))

    if not found:
        raise Exception("generate_string_conversion: Failed to find {} in {}"
                        .format(define_regex, srcfile))

    out.write('    }\n')
    out.write('\n')
    out.write('    return ("{}_INVALID");\n'.format(name.upper()))
    out.write('}\n')

if __name__ == '__main__':
    src = '../src/include/str_inline.h'
    tmp = '__tmp_type_to_str' + str(os.getpid())

    with open(tmp, 'w') as tfile:
        tfile.write("#pragma once\n\n")
        generate_string_conversion('Convert a prepare state to its string representation.',
                                   'prepare_state',
                                   r'^#define\s+(WT_PREPARE_[A-Z0-9_]+).*?[0-9]+',
                                   'src/include/btmem.h',
                                   tfile)
        tfile.write('\n')

        generate_string_conversion('Convert an update type to its string representation.',
                                   'update_type',
                                   # Also match comments/space at end to avoid e.g. WT_UPDATE_SIZE.
                                   r'^#define\s+(WT_UPDATE_[A-Z0-9_]+)\s+[0-9]+\s+/\*',
                                   'src/include/btmem.h',
                                   tfile)
        tfile.write('\n')

        generate_string_conversion('Convert a page type to its string representation.',
                                   'page_type',
                                   r'^#define\s+(WT_PAGE_(?!.*VERSION)[A-Z0-9_]+)\s+[0-9]+\s+/\*',
                                   'src/include/btmem.h',
                                   tfile)

    format_srcfile(tmp)
    compare_srcfile(tmp, src)

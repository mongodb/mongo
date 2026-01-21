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

from py_common import binary_data

# Manages printing to output.
# We keep track of cells, the first line printed for a new cell
# shows the cell number, subsequent lines are indented a little.
# If the split option is on, we show any bytes that were used
# in decoding before the regular decoding output appears.
# Those 'input bytes' are shown shifted to the right.
class Printer(object):
    def __init__(self, binfile, opts):
        self.binfile = binfile
        self.issplit = opts.split
        self.verbose = opts.verbose
        self.ext = opts.ext
        self.cellpfx = ''
        self.in_cell = False

    def begin_cell(self, cell_number):
        self.cellpfx = f'{cell_number}: '
        self.in_cell = True
        ignore = self.binfile.saved_bytes()  # reset the saved position

    def end_cell(self):
        self.in_cell = False
        self.cellpfx = ''

    # This is the 'print' function, used as p.rint()
    def rint(self, s):
        if self.issplit:
            saved_bytes = self.binfile.saved_bytes()[:]
            # For the split view, we want to have the bytes related to
            # stuff to be normally printed to appear indented by 40 spaces,
            # with 10 more spaces to show a possibly abbreviated file position.
            # If we are beginning a cell, we want that to appear left justified,
            # within the 40 spaces of indentation.
            if len(saved_bytes) > 0:
                # create the 10 character file position
                # the current file position has actually advanced by
                # some number of bytes, so subtract that now.
                cur_pos = self.binfile.tell() - len(saved_bytes)
                file_pos = f'{cur_pos:x}'
                if len(file_pos) > 8:
                    file_pos = '...' + file_pos[-5:]
                elif len(file_pos) < 8:
                    file_pos = ' ' * (8 - len(file_pos)) + file_pos
                file_pos += ': '

                indentation = (self.cellpfx + ' ' * 40)[0:40]
                self.cellpfx = ''
                while len(saved_bytes) > 20:
                    print(indentation + file_pos + str(saved_bytes[:20].hex(' ')))
                    saved_bytes = saved_bytes[20:]
                    indentation = ' ' * 40
                    file_pos = ' ' * 10
                print(indentation + file_pos + str(saved_bytes.hex(' ')))

        pfx = self.cellpfx
        self.cellpfx = ''
        if pfx == '' and self.in_cell:
            pfx = '  '
        print(pfx + str(s))

    def rint_v(self, s):
        if self.verbose:
            self.rint(s)

    def rint_ext(self, s):
        if self.ext:
            self.rint(s)
            
def raw_bytes(b):
    if type(b) != type(b''):
        # Not bytes, it's already a string.
        return b

    # If the high bit of the first byte is on, it's likely we have
    # a packed integer.  If the high bit is off, it's possible we have
    # a packed integer (it would be negative) but it's harder to guess,
    # we'll presume a string.  But if the byte is 0x7f, that's ASCII DEL,
    # very unlikely to be the beginning of a string, but it decodes as -1,
    # so seems more likely to be an int.  If the UTF-8 decoding of the
    # string fails, we probably just have binary data.

    # Try decoding as one or more packed ints
    result = ''
    s = b
    while len(s) > 0 and s[0] >= 0x7f:
        val, s = binary_data.unpack_int(s)
        if result != '':
            result += ' '
        result += f'<packed {binary_data.d_and_h(val)}>'
    if len(s) == 0:
        return result

    # See if the rest of the bytes can be decoded as a string
    try:
        if result != '':
            result += ' '
        return f'"{result + s.decode()}"'
    except:
        pass

    # The earlier steps failed, so it must be binary data
    return binary_to_pretty_string(b, start_with_line_prefix=False)

# Convert binary data to a multi-line string with hex and printable characters
def binary_to_pretty_string(b, per_line=16, line_prefix='  ', start_with_line_prefix=True):
    printable = ''
    result = ''
    if start_with_line_prefix:
        result += line_prefix
    if len(b) == 0:
        return result
    for i in range(0, len(b)):
        if i > 0:
            if i % per_line == 0:
                result += '  ' + printable + '\n' + line_prefix
                printable = ''
            else:
                result += ' '
        result += '%02x' % b[i]
        if b[i] >= ord(' ') and b[i] < 0x7f:
            printable += chr(b[i])
        else:
            printable += '.'
    if i % per_line != per_line - 1:
        for j in range(i % per_line + 1, per_line):
            result += '   '
    result += '  ' + printable
    return result

def dumpraw(p, b, pos):
    savepos = b.tell()
    b.seek(pos)
    i = 0
    per_line = 16
    s = binary_to_pretty_string(b.read(256), per_line=per_line, line_prefix='')
    for line in s.splitlines():
        p.rint_v(hex(pos + i) + ':  ' + line)
        i += per_line
    b.seek(savepos)

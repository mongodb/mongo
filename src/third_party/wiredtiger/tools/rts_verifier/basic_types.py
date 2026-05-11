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

from enum import Enum

class PrepareState(Enum):
    WT_PREPARE_INIT = 0
    WT_PREPARE_INPROGRESS = 1
    WT_PREPARE_LOCKED = 2
    WT_PREPARE_RESOLVED = 3

class UpdateType(Enum):
    WT_UPDATE_MODIFY = 0
    WT_UPDATE_RESERVE = 1
    WT_UPDATE_STANDARD = 2
    WT_UPDATE_TOMBSTONE = 3

class PageType(Enum):
    WT_PAGE_INVALID = 0
    WT_PAGE_BLOCK_MANAGER = 1
    WT_PAGE_COL_INT = 3
    WT_PAGE_COL_VAR = 4
    WT_PAGE_OVFL = 5
    WT_PAGE_ROW_INT = 6
    WT_PAGE_ROW_LEAF = 7

class Timestamp():
    def __init__(self, start, stop):
        self.start = start
        self.stop = stop

    def __eq__(self, other):
        return self.start == other.start and self.stop == other.stop

    def __lt__(self, rhs):
        return ((self.start, self.stop) < (rhs.start, rhs.stop))

    def __le__(self, rhs):
        return self.__lt__(rhs) or self.__eq__(rhs)

    def __gt__(self, rhs):
        return ((self.start, self.stop) > (rhs.start, rhs.stop))

    def __ge__(self, rhs):
        return self.__gt__(rhs) or self.__eq__(rhs)

    def __repr__(self):
        return f"({self.start}, {self.stop})"

class Tree():
    def __init__(self, filename):
        self.logged = None
        self.file = filename

    def __eq__(self, other):
        return self.file == other.file

    def __hash__(self):
        return hash(self.file)

class Page():
    def __init__(self, addr):
        self.addr = addr
        self.modified = None

    def __eq__(self, other):
        return self.addr == other.addr

    def __hash__(self):
        return hash(self.addr)

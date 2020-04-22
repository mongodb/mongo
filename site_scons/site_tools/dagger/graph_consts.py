# Copyright 2020 MongoDB Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

"""Constants for use in graph.py and dagger.py"""
"""Relationship edge types"""
LIB_LIB = 1
LIB_FIL = 2
FIL_LIB = 3
FIL_FIL = 4
FIL_SYM = 5
LIB_SYM = 6
IMP_LIB_LIB = 7
EXE_LIB = 8


"""NodeTypes"""
NODE_LIB = 1
NODE_SYM = 2
NODE_FILE = 3
NODE_EXE = 4

RELATIONSHIP_TYPES = list(range(1, 9))
NODE_TYPES = list(range(1, 5))


"""Error/query codes"""
NODE_NOT_FOUND = 1

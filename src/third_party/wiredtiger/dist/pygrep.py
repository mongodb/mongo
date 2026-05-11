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

# This a simple limited replacement for GNU grep
# Caveats:
# * Accepts only stdin
# * Unknown options must go as a separate arg

import re, sys, argparse

def matchLine(regex, line):
    global args
    match = regex.search(line)
    if bool(match) != bool(args.inverse): print(line, end='')

def matchSearch(regex, line):
    for match in regex.finditer(line):
        print(match if isinstance(match, str) else match[0])

argparser = argparse.ArgumentParser(description='Simple Grep Replacement.', add_help=False)
argparser.add_argument('-F', '--fgrep', dest='is_regex', action='store_false', default=True,
        help='Interpret pattern as a set of fixed strings (behave like fgrep).')
argparser.add_argument('-E', '--egrep', dest='is_regex', action='store_true',
        help='Interpret pattern as an extended regular expression (behave like egrep).')
argparser.add_argument('-e', '--regexp', dest='pattern', action='append',
        help='Specify multiple patterns on the command line.')
argparser.add_argument('-w', '--word-regexp', dest='word', action='store_true',
        help='The expression is searched for as a word.')
argparser.add_argument('-v', '--invert-match', dest='inverse', action='store_true',
        help='Selected lines are those not matching any of the specified patterns.')
argparser.add_argument('-o', '--only-matching', dest='only_matching', action='store_true',
        help='Prints only the matching part of the lines.')
argparser.add_argument('pattern', action='append', nargs='?',
        help='Search pattern.')

# Ignore unknown options rather than complain
args = argparser.parse_known_args(sys.argv[1:])[0]
args.pattern = [line
                for pat in args.pattern if pat
                for line in pat.splitlines() if line]
if not args.pattern:
    argparser.print_help()
    sys.exit(0)

match = matchSearch if args.only_matching else matchLine
if not args.is_regex:
    args.pattern = [re.escape(pat) for pat in args.pattern]
regex = '|'.join(['(?:' + pat + ')' for pat in args.pattern])
if args.word:
    regex = '\\b(?:' + regex + ')\\b'

regex = re.compile(regex)

for line in sys.stdin:
    match(regex, line)


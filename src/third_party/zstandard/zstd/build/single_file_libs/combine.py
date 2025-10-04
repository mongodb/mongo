#!/usr/bin/env python3

# Tool to bundle multiple C/C++ source files, inlining any includes.
# 
# Note: there are two types of exclusion options: the '-x' flag, which besides
# excluding a file also adds an #error directive in place of the #include, and
# the '-k' flag, which keeps the #include and doesn't inline the file. The
# intended use cases are: '-x' for files that would normally be #if'd out, so
# features that 100% won't be used in the amalgamated file, for which every
# occurrence adds the error, and '-k' for headers that we wish to manually
# include, such as a project's public API, for which occurrences after the first
# are removed.
# 
# Todo: the error handling could be better, which currently throws and halts
# (which is functional just not very friendly).
# 
# Author: Carl Woffenden, Numfum GmbH (this script is released under a CC0 license/Public Domain)

import argparse, re, sys

from pathlib import Path
from typing import Any, List, Optional, Pattern, Set, TextIO

# Set of file roots when searching (equivalent to -I paths for the compiler).
roots: Set[Path] = set()

# Set of (canonical) file Path objects to exclude from inlining (and not only
# exclude but to add a compiler error directive when they're encountered).
excludes: Set[Path] = set()

# Set of (canonical) file Path objects to keep as include directives.
keeps: Set[Path] = set()

# Whether to keep the #pragma once directives (unlikely, since this will result
# in a warning, but the option is there).
keep_pragma: bool = False

# Destination file object (or stdout if no output file was supplied).
destn: TextIO = sys.stdout

# Set of file Path objects previously inlined (and to ignore if reencountering).
found: Set[Path] = set()

# Compiled regex Pattern to handle "#pragma once" in various formats:
# 
#   #pragma once
#     #pragma once
#   #  pragma once
#   #pragma   once
#   #pragma once // comment
# 
# Ignoring commented versions, same as include_regex.
# 
pragma_regex: Pattern = re.compile(r'^\s*#\s*pragma\s*once\s*')

# Compiled regex Pattern to handle the following type of file includes:
# 
#   #include "file"
#     #include "file"
#   #  include "file"
#   #include   "file"
#   #include "file" // comment
#   #include "file" // comment with quote "
# 
# And all combinations of, as well as ignoring the following:
# 
#   #include <file>
#   //#include "file"
#   /*#include "file"*/
# 
# We don't try to catch errors since the compiler will do this (and the code is
# expected to be valid before processing) and we don't care what follows the
# file (whether it's a valid comment or not, since anything after the quoted
# string is ignored)
# 
include_regex: Pattern = re.compile(r'^\s*#\s*include\s*"(.+?)"')

# Simple tests to prove include_regex's cases.
# 
def test_match_include() -> bool:
    if (include_regex.match('#include "file"')   and
        include_regex.match('  #include "file"') and
        include_regex.match('#  include "file"') and
        include_regex.match('#include   "file"') and
        include_regex.match('#include "file" // comment')):
            if (not include_regex.match('#include <file>')   and
                not include_regex.match('//#include "file"') and
                not include_regex.match('/*#include "file"*/')):
                    found = include_regex.match('#include "file" // "')
                    if (found and found.group(1) == 'file'):
                        print('#include match valid')
                        return True
    return False

# Simple tests to prove pragma_regex's cases.
# 
def test_match_pragma() -> bool:
    if (pragma_regex.match('#pragma once')   and
        pragma_regex.match('  #pragma once') and
        pragma_regex.match('#  pragma once') and
        pragma_regex.match('#pragma   once') and
        pragma_regex.match('#pragma once // comment')):
            if (not pragma_regex.match('//#pragma once') and
                not pragma_regex.match('/*#pragma once*/')):
                    print('#pragma once match valid')
                    return True
    return False

# Finds 'file'. First the list of 'root' paths are searched, followed by the
# currently processing file's 'parent' path, returning a valid Path in
# canonical form. If no match is found None is returned.
# 
def resolve_include(file: str, parent: Optional[Path] = None) -> Optional[Path]:
    for root in roots:
        found = root.joinpath(file).resolve()
        if (found.is_file()):
            return found
    if (parent):
        found = parent.joinpath(file).resolve();
    else:
        found = Path(file)
    if (found.is_file()):
        return found
    return None

# Helper to resolve lists of files. 'file_list' is passed in from the arguments
# and each entry resolved to its canonical path (like any include entry, either
# from the list of root paths or the owning file's 'parent', which in this case
# is case is the input file). The results are stored in 'resolved'.
# 
def resolve_excluded_files(file_list: Optional[List[str]], resolved: Set[Path], parent: Optional[Path] = None) -> None:
    if (file_list):
        for filename in file_list:
            found = resolve_include(filename, parent)
            if (found):
                resolved.add(found)
            else:
                error_line(f'Warning: excluded file not found: {filename}')

# Writes 'line' to the open 'destn' (or stdout).
# 
def write_line(line: str) -> None:
    print(line, file=destn)

# Logs 'line' to stderr. This is also used for general notifications that we
# don't want to go to stdout (so the source can be piped).
# 
def error_line(line: Any) -> None:
    print(line, file=sys.stderr)

# Inline the contents of 'file' (with any of its includes also inlined, etc.).
# 
# Note: text encoding errors are ignored and replaced with ? when reading the
# input files. This isn't ideal, but it's more than likely in the comments than
# code and a) the text editor has probably also failed to read the same content,
# and b) the compiler probably did too.
# 
def add_file(file: Path, file_name: str = None) -> None:
    if (file.is_file()):
        if (not file_name):
            file_name = file.name
        error_line(f'Processing: {file_name}')
        with file.open('r', errors='replace') as opened:
            for line in opened:
                line = line.rstrip('\n')
                match_include = include_regex.match(line);
                if (match_include):
                    # We have a quoted include directive so grab the file
                    inc_name = match_include.group(1)
                    resolved = resolve_include(inc_name, file.parent)
                    if (resolved):
                        if (resolved in excludes):
                            # The file was excluded so error if the compiler uses it
                            write_line(f'#error Using excluded file: {inc_name} (re-amalgamate source to fix)')
                            error_line(f'Excluding: {inc_name}')
                        else:
                            if (resolved not in found):
                                # The file was not previously encountered
                                found.add(resolved)
                                if (resolved in keeps):
                                    # But the include was flagged to keep as included
                                    write_line(f'/**** *NOT* inlining {inc_name} ****/')
                                    write_line(line)
                                    error_line(f'Not inlining: {inc_name}')
                                else:
                                    # The file was neither excluded nor seen before so inline it
                                    write_line(f'/**** start inlining {inc_name} ****/')
                                    add_file(resolved, inc_name)
                                    write_line(f'/**** ended inlining {inc_name} ****/')
                            else:
                                write_line(f'/**** skipping file: {inc_name} ****/')
                    else:
                        # The include file didn't resolve to a file
                        write_line(f'#error Unable to find: {inc_name}')
                        error_line(f'Error: Unable to find: {inc_name}')
                else:
                    # Skip any 'pragma once' directives, otherwise write the source line
                    if (keep_pragma or not pragma_regex.match(line)):
                        write_line(line)
    else:
        error_line(f'Error: Invalid file: {file}')

# Start here
parser = argparse.ArgumentParser(description='Amalgamate Tool', epilog=f'example: {sys.argv[0]} -r ../my/path -r ../other/path -o out.c in.c')
parser.add_argument('-r', '--root', action='append', type=Path, help='file root search path')
parser.add_argument('-x', '--exclude',  action='append', help='file to completely exclude from inlining')
parser.add_argument('-k', '--keep', action='append', help='file to exclude from inlining but keep the include directive')
parser.add_argument('-p', '--pragma', action='store_true', default=False, help='keep any "#pragma once" directives (removed by default)')
parser.add_argument('-o', '--output', type=argparse.FileType('w'), help='output file (otherwise stdout)')
parser.add_argument('input', type=Path, help='input file')
args = parser.parse_args()

# Fail early on an invalid input (and store it so we don't recurse)
args.input = args.input.resolve(strict=True)
found.add(args.input)

# Resolve all of the root paths upfront (we'll halt here on invalid roots)
if (args.root):
    for path in args.root:
        roots.add(path.resolve(strict=True))

# The remaining params: so resolve the excluded files and #pragma once directive
resolve_excluded_files(args.exclude, excludes, args.input.parent)
resolve_excluded_files(args.keep,    keeps,    args.input.parent)
keep_pragma = args.pragma;

# Then recursively process the input file
try:
    if (args.output):
        destn = args.output
    add_file(args.input)
finally:
    if (destn):
        destn.close()

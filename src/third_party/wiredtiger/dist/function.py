#!/usr/bin/env python

# Check the style of WiredTiger C code.
from __future__ import print_function
import fnmatch, os, re, sys
from dist import all_c_files, compare_srcfile, source_files

# Complain if a function comment is missing.
def missing_comment():
    for f in source_files():
        skip_re = re.compile(r'DO NOT EDIT: automatically built')
        func_re = re.compile(
            r'(/\*(?:[^\*]|\*[^/])*\*/)?\n\w[\w \*]+\n(\w+)', re.DOTALL)
        s = open(f, 'r').read()
        if skip_re.search(s):
            continue
        for m in func_re.finditer(s):
            if not m.group(1) or \
               not m.group(1).startswith('/*\n * %s --\n' % m.group(2)):
                   print("%s:%d: missing or malformed comment for %s" % \
                           (f, s[:m.start(2)].count('\n'), m.group(2)))

# Sort helper function, discard * operators so a pointer doesn't necessarily
# sort before non-pointers, ignore const/static/volatile keywords.
def function_args_alpha(text):
        s = text.strip()
        s = re.sub("[*]","", s)
        s = s.split()
        def merge_specifier(words, specifier):
            if len(words) > 2 and words[0] == specifier:
                words[1] += specifier
                words = words[1:]
            return words
        s = merge_specifier(s, 'const')
        s = merge_specifier(s, 'static')
        s = merge_specifier(s, 'volatile')
        s = ' '.join(s)
        return s

# List of illegal types.
illegal_types = [
    'u_int16_t',
    'u_int32_t',
    'u_int64_t',
    'u_int8_t',
    'u_quad',
    'uint '
]

# List of legal types in sort order.
types = [
    'struct',
    'union',
    'enum',
    'DIR',
    'FILE',
    'TEST_',
    'WT_',
    'wt_',
    'DWORD',
    'double',
    'float',
    'intmax_t',
    'intptr_t',
    'clock_t',
    'pid_t',
    'pthread_t',
    'size_t',
    'ssize_t',
    'time_t',
    'uintmax_t',
    'uintptr_t',
    'u_long',
    'long',
    'uint64_t',
    'int64_t',
    'uint32_t',
    'int32_t',
    'uint16_t',
    'int16_t',
    'uint8_t',
    'int8_t',
    'u_int',
    'int',
    'u_char',
    'char',
    'bool',
    'va_list',
    'void '
]

# Return the sort order of a variable declaration, or no-match.
#       This order isn't defensible: it's roughly how WiredTiger looked when we
# settled on a style, and it's roughly what the KNF/BSD styles look like.
def function_args(name, line):
    line = line.strip()
    line = re.sub("^const ", "", line)
    line = re.sub("^static ", "", line)
    line = re.sub("^volatile ", "", line)

    # Let WT_ASSERT, WT_UNUSED and WT_RET terminate the parse. They often appear
    # at the beginning of the function and looks like a WT_XXX variable
    # declaration.
    if re.search('^WT_ASSERT', line):
        return False,0
    if re.search('^WT_UNUSED', line):
        return False,0
    if re.search('^WT_RET', line):
        return False,0

    # Let lines not terminated with a semicolon terminate the parse, it means
    # there's some kind of interesting line split we probably can't handle.
    if not re.search(';$', line):
        return False,0

    # Check for illegal types.
    for m in illegal_types:
        if re.search('^' + m + "\s*[\w(*]", line):
            print(name + ": illegal type: " + line.strip(), file=sys.stderr)
            sys.exit(1)

    # Check for matching types.
    for n,m in enumerate(types, 0):
        # Don't list '{' as a legal character in a declaration, that's what
        # prevents us from sorting inline union/struct declarations.
        if re.search('^' + m + "\s*[\w(*]", line):
            return True,n
    return False,0

# Put function arguments in correct sort order.
def function_declaration():
    tmp_file = '__tmp'
    for name in all_c_files():
        skip_re = re.compile(r'DO NOT EDIT: automatically built')
        s = open(name, 'r').read()
        if skip_re.search(s):
            continue

        # Read through the file, and for each function, do a style pass over
        # the local declarations. Quit tracking declarations as soon as we
        # find anything we don't understand, leaving it untouched.
        with open(name, 'r') as f:
            tfile = open(tmp_file, 'w')
            tracking = False
            for line in f:
                if not tracking:
                    tfile.write(line)
                    if re.search('^{$', line):
                        list = [[] for i in range(len(types))]
                        static_list = [[] for i in range(len(types))]
                        tracking = True;
                    continue

                found,n = function_args(name, line)
                if found:
                    # List statics first.
                    if re.search("^\s+static", line):
                        static_list[n].append(line)
                        continue

                    # Disallow assignments in the declaration. Ignore braces
                    # to allow automatic array initialization using constant
                    # initializers (and we've already skipped statics, which
                    # are also typically initialized in the declaration).
                    if re.search("\s=\s[-\w]", line):
                        print(name + ": assignment in string: " + line.strip(),\
                              file=sys.stderr)
                        sys.exit(1);

                    list[n].append(line)
                else:
                    # Sort the resulting lines (we don't yet sort declarations
                    # within a single line). It's two passes, first to catch
                    # the statics, then to catch everything else.
                    for arg in filter(None, static_list):
                        for p in sorted(arg, key=function_args_alpha):
                            tfile.write(p)
                    for arg in filter(None, list):
                        for p in sorted(arg, key=function_args_alpha):
                            tfile.write(p)
                    tfile.write(line)
                    tracking = False
                    continue

            tfile.close()
            compare_srcfile(tmp_file, name)

# Report missing function comments.
missing_comment()

# Update function argument declarations.
function_declaration()

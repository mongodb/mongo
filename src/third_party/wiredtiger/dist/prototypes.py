#!/usr/bin/env python

# Generate WiredTiger function prototypes.
import fnmatch, re
from dist import compare_srcfile, format_srcfile, source_files

# Build function prototypes from a list of files.
def prototypes(list, name):
    s = open(name, 'r').read()
    for p in re.findall(r'\n[A-Za-z_].*\n__wt_[^{]*', s):
        l = p.strip()

        # Ignore statics in XXX.c files.
        if fnmatch.fnmatch(name, "*.c") and 'static' in l:
                continue

        # Join the first two lines, type and function name.
        l = l.replace("\n", " ", 1)

        # If there's no CPP syntax, join everything.
        if not '#endif' in l:
            l = " ".join(l.split())

        # If it's not an inline function, prefix with "extern".
        if not 'inline' in l:
                l = 'extern ' + l

        # Switch to the include file version of any gcc attributes.
        l = l.replace("WT_GCC_FUNC_ATTRIBUTE", "WT_GCC_FUNC_DECL_ATTRIBUTE")

        # Everything but void requires using any return value.
        if not re.match(r'(static inline|extern) void', l):
                l = l + " WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))"

        # If a line ends in #endif, appending a semicolon results in an illegal
        # expression, force an appended newline.
        if re.match(r'#endif$', l):
            l = l + '\n'

        # Add the trailing semi-colon.
        list.append(l + ';\n')

# Write results and compare to the current file.
def output(p, f):
    tmp_file = '__tmp'
    tfile = open(tmp_file, 'w')
    for e in sorted(list(set(p))):
        tfile.write(e)
    tfile.close()
    format_srcfile(tmp_file)
    compare_srcfile(tmp_file, f)

# Update generic function prototypes.
def prototypes_extern():
    p = []
    for name in source_files():
        if not fnmatch.fnmatch(name, '*.c') + fnmatch.fnmatch(name, '*_inline.h'):
                continue;
        if fnmatch.fnmatch(name, '*/checksum/arm64/*'):
            continue
        if fnmatch.fnmatch(name, '*/checksum/power8/*'):
            continue
        if fnmatch.fnmatch(name, '*/checksum/riscv64/*'):
            continue
        if fnmatch.fnmatch(name, '*/checksum/zseries/*'):
            continue
        if fnmatch.fnmatch(name, '*/os_posix/*'):
            continue
        if fnmatch.fnmatch(name, '*/os_win/*'):
            continue
        if fnmatch.fnmatch(name, '*/ext/*'):
            continue
        prototypes(p, name)

    output(p, "../src/include/extern.h")

# Update POSIX-specific function prototypes.
def prototypes_posix():
    p = []
    for name in source_files():
        if not fnmatch.fnmatch(name, '*.c') + fnmatch.fnmatch(name, '*_inline.h'):
                continue;
        if not fnmatch.fnmatch(name, '*/os_posix/*'):
            continue
        prototypes(p, name)

    output(p, "../src/include/extern_posix.h")

# Update Windows-specific function prototypes.
def prototypes_win():
    p = []
    for name in source_files():
        if not fnmatch.fnmatch(name, '*.c') + fnmatch.fnmatch(name, '*_inline.h'):
                continue;
        if not fnmatch.fnmatch(name, '*/os_win/*'):
            continue
        prototypes(p, name)

    output(p, "../src/include/extern_win.h")

prototypes_extern()
prototypes_posix()
prototypes_win()

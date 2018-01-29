#!/usr/bin/env python

import re, sys
from dist import all_c_files, all_h_files, compare_srcfile

# Automatically build flags values: read through all of the header files, and
# for each group of flags, sort them and give them a unique value.
#
# To add a new flag declare it at the top of the flags list as:
# #define WT_NEW_FLAG_NAME      0x0u
#
# and it will be automatically alphabetized and assigned the proper value.
def flag_declare(name):
    tmp_file = '__tmp'
    with open(name, 'r') as f:
        tfile = open(tmp_file, 'w')

        lcnt = 0
        parsing = False
        for line in f:
            lcnt = lcnt + 1
            if line.find('AUTOMATIC FLAG VALUE GENERATION START') != -1:
                header = line
                defines = []
                parsing = True
            elif line.find('AUTOMATIC FLAG VALUE GENERATION STOP') != -1:
                # We only support 64 bits.
                if len(defines) > 64:
                    print >>sys.stderr, name + ": line " +\
                        str(lcnt) + ": exceeds maximum 64 bit flags"
                    sys.exit(1)

                # Calculate number of hex bytes, create format string
                fmt = "0x%%0%dxu" % ((len(defines) + 3) / 4)

                tfile.write(header)
                v = 1
                for d in sorted(defines):
                    tfile.write(re.sub("0x[01248u]*", fmt % v, d))
                    v = v * 2
                tfile.write(line)

                parsing = False
            elif parsing and line.find('#define') == -1:
                print >>sys.stderr, name + ": line " +\
                    str(lcnt) + ": unexpected flag line, no #define"
                sys.exit(1)
            elif parsing:
                defines.append(line)
            else:
                tfile.write(line)

        tfile.close()
        compare_srcfile(tmp_file, name)

# Update function argument declarations.
for name in all_h_files():
    flag_declare(name)
for name in all_c_files():
    flag_declare(name)

#!/usr/bin/env python

from __future__ import print_function
import re, sys
from dist import all_c_files, all_h_files, compare_srcfile

# Automatically build flags values: read through all of the header files, and
# for each group of flags, sort them, check the start and stop boundaries on
# the flags and give them a unique value.
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
        max_flags = 0
        parsing = False
        start = 0
        stopped = ''
        for line in f:
            lcnt = lcnt + 1
            if stopped != '':
                stopped = ''
                m = re.search("\d+", line)
                if m != None:
                    fld_size = int(m.group(0))
                    if max_flags > fld_size:
                        print("file: " + name + " line: " + str(lcnt))
                        print("Flag stop value of " + str(max_flags) \
                                + " is larger than field size " + str(fld_size))
                        sys.exit(1)
            if line.find('AUTOMATIC FLAG VALUE GENERATION START') != -1:
                m = re.search("\d+", line)
                if m == None:
                    print(name + ": automatic flag generation start at line " +
                        str(lcnt) + " needs start value e.g. AUTOMATIC FLAG VALUE" +
                        " GENERATION START 0", file=sys.stderr)
                    sys.exit(1)
                start = int(m.group(0))
                header = line
                defines = []
                parsing = True
            elif line.find('AUTOMATIC FLAG VALUE GENERATION STOP') != -1:
                m = re.search("\d+", line)
                if m == None:
                    print(name + ": automatic flag generation stop at line " +
                        str(lcnt) + " needs stop value e.g. AUTOMATIC FLAG VALUE" +
                        " GENERATION STOP 32", file=sys.stderr)
                    sys.exit(1)
                end = int(m.group(0))

                poweroftwo = (end != 0) and ((end & (end-1)) == 0)
                if not poweroftwo and end != 12:
                    print(name + ": line " + str(lcnt) + ", the stop value " + str(end) +
                    " is not a power of 2", file=sys.stderr)
                    sys.exit(1)
                # Compare the number of flags defined and against the number
                # of flags allowed
                max_flags = len(defines)
                if max_flags > end - start:
                    print(name + ": line " + str(lcnt) +\
                          ": exceeds maximum {0} limit bit flags".format(end), file=sys.stderr)
                    sys.exit(1)

                # Calculate number of hex bytes, create format string
                if end <= 32:
                    fmt = "0x%%0%dxu" % ((start + max_flags + 3) / 4)
                else:
                    fmt = "0x%%0%dxull" % ((start + max_flags + 3) / 4)

                # Generate the flags starting from an offset set from the start value.
                tfile.write(header)
                v = 1 << start
                for d in sorted(defines):
                    tfile.write(re.sub("0x[01248]*ul*", fmt % v, d))
                    v = v * 2
                tfile.write(line)

                parsing = False
                start = 0
                stopped = line
                continue
            elif parsing and line.find('#define') == -1:
                print(name + ": line " + str(lcnt) +\
                      ": unexpected flag line, no #define", file=sys.stderr)
                sys.exit(1)
            elif parsing:
                defines.append(line)
            else:
                tfile.write(line)
            stopped = ''

        tfile.close()
        compare_srcfile(tmp_file, name)

# Update function argument declarations.
for name in all_h_files():
    flag_declare(name)
for name in all_c_files():
    flag_declare(name)

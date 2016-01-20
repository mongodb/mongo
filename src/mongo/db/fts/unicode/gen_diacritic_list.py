#!/usr/bin/env python
# -*- coding: utf-8 -*-
import sys

from gen_helper import getCopyrightNotice, openNamespaces, closeNamespaces, \
    include

def generate(unicode_proplist_file, target):
    """Generates a C++ source file that contains a diacritic checking function.

    The diacritic checking function contains a switch statement with cases for 
    every diacritic in the Unicode Character Database.
    """
    out = open(target, "w")

    out.write(getCopyrightNotice())
    out.write(include("mongo/db/fts/unicode/codepoints.h"))
    out.write("\n")
    out.write(openNamespaces())

    diacritics = set()

    proplist_file = open(unicode_proplist_file, 'rU')

    for line in proplist_file:
        # Filter out blank lines and lines that start with #
        data = line[:line.find('#')]
        if(data == ""):
            continue

        # Parse the data on the line
        values = data.split("; ")
        assert(len(values) == 2)

        uproperty = values[1].strip()
        if uproperty in "Diacritic":
            if len(values[0].split('..')) == 2:
                codepoint_range = values[0].split('..')

                start = int(codepoint_range[0], 16)
                end   = int(codepoint_range[1], 16) + 1

                for i in range(start, end):
                    if i not in diacritics: 
                        diacritics.add(i)
            else:
                if int(values[0], 16) not in diacritics:
                    diacritics.add(int(values[0], 16))

    out.write("""bool codepointIsDiacritic(char32_t codepoint) {
    switch (codepoint) {\n""")

    for diacritic in sorted(diacritics):
        out.write("\
    case " + str(hex(diacritic)) + ": return true;\n")

    out.write("\
    default: return false;\n    }\n}")

    out.write(closeNamespaces())

if __name__ == "__main__":
    generate(sys.argv[1], sys.argv[2])

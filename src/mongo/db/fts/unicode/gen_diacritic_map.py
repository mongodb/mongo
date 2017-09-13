#!/usr/bin/env python
# -*- coding: utf-8 -*-
import sys
from unicodedata import normalize, category, unidata_version

from gen_helper import getCopyrightNotice, openNamespaces, closeNamespaces, \
    include

diacritics = set()

def load_diacritics(unicode_proplist_file):
    proplist_file = open(unicode_proplist_file, 'r')

    for line in proplist_file:
        # Filter out blank lines and lines that start with #
        data = line[:line.find('#')]
        if(data == ""):
            continue

        # Parse the data on the line
        values = data.split("; ")
        assert(len(values) == 2)

        uproperty = values[1].strip()
        if uproperty == "Diacritic":
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

diacritic_mappings = {}

def add_diacritic_mapping(codepoint):
    # a : original unicode character
    # d : decomposed unicode character
    # r : decomposed unicode character with diacritics removed
    # c : recomposed unicode character with diacritics removed
    a = chr(codepoint)
    d = normalize('NFD', a)
    r = u''

    for i in range(len(d)):
        if ord(d[i]) not in diacritics:
            r += d[i]

    c = normalize('NFC', r)

    # Only use mappings where the final recomposed form is a single codepoint
    if (a != c and len(c) == 1):
        assert c != '\0' # This is used to indicate the codepoint is a pure diacritic.
        assert ord(c) not in diacritics
        diacritic_mappings[codepoint] = ord(c[0])

def add_diacritic_range(start, end):
    for x in range(start, end + 1):
        add_diacritic_mapping(x)

def generate(target):
    """Generates a C++ source file that contains a diacritic removal mapping 
       function.

    The delimiter checking function contains a switch statement with cases for
    every character in Unicode that has a removable combining diacritical mark.
    """
    out = open(target, "w")

    out.write(getCopyrightNotice())
    out.write(include("mongo/db/fts/unicode/codepoints.h"))
    out.write("\n")
    out.write(openNamespaces())

    # Map diacritics from 0 to the maximum Unicode codepoint
    add_diacritic_range(0x0000, 0x10FFFF)

    for diacritic in diacritics:
        diacritic_mappings[diacritic] = 0

    out.write("""char32_t codepointRemoveDiacritics(char32_t codepoint) {
    switch (codepoint) {\n""")

    mappings_list = []

    for mapping in diacritic_mappings:
        mappings_list.append((mapping, diacritic_mappings[mapping]))

    sorted_mappings = sorted(mappings_list, key=lambda mapping: mapping[0])

    for mapping in sorted_mappings:
        out.write("    case " + str(hex(mapping[0])) + ": return " + \
            str(hex(mapping[1])) +";\n")

    out.write("    default: return codepoint;\n    }\n}")

    out.write(closeNamespaces())

if __name__ == "__main__":
    if(unidata_version != '8.0.0'):
        print("""ERROR: This script must be run with a version of Python that \
            contains the Unicode 8.0.0 Character Database.""")
        sys.exit(1)
    load_diacritics(sys.argv[1])
    generate(sys.argv[2])

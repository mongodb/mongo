#!/usr/bin/env python
# -*- coding: utf-8 -*-
import os
import sys

from gen_helper import getCopyrightNotice, openNamespaces, closeNamespaces, \
    include

def generate(unicode_casefold_file, target):
    """Generates a C++ source file that contains a Unicode case folding
       function.

    The case folding function contains a switch statement with cases for every
    Unicode codepoint that has a case folding mapping.
    """
    out = open(target, "w")

    out.write(getCopyrightNotice())
    out.write(include("mongo/db/fts/unicode/codepoints.h"))
    out.write("\n")
    out.write(openNamespaces())

    case_mappings = {}

    cf_file = open(unicode_casefold_file, 'rU')

    for line in cf_file:
        # Filter out blank lines and lines that start with #
        data = line[:line.find('#')]
        if(data == ""):
            continue

        # Parse the data on the line
        values = data.split("; ")
        assert(len(values) == 4)

        status = values[1]
        if status == 'C' or status == 'S':
            # We only include the "Common" and "Simple" mappings. "Full" case 
            # folding mappings expand certain letters to multiple codepoints, 
            # which we currently do not support.
            original_codepoint = int(values[0], 16)
            codepoint_mapping  = int(values[2], 16)
            case_mappings[original_codepoint] = codepoint_mapping

    turkishMapping = {
        0x49: 0x131,  # I -> ı
        0x130: 0x069,   # İ -> i
    }

    out.write(
        """char32_t codepointToLower(char32_t codepoint, CaseFoldMode mode) {
               if (codepoint <= 0x7f) {
                    if (codepoint >= 'A' && codepoint <= 'Z') {
                       return (mode == CaseFoldMode::kTurkish && codepoint == 'I')
                              ? 0x131
                              : (codepoint | 0x20); // Set the ascii lowercase bit on the character.
                   }
                   return codepoint;
               }

               switch (codepoint) {\n""")

    mappings_list = []

    for mapping in case_mappings:
        mappings_list.append((mapping, case_mappings[mapping]))

    # Make sure we include each mapping in turkishMapping in the cases below. This ensures we handle
    # them even if we'd skip the letter in non-turkish mode.
    for mapping in turkishMapping:
        if mapping not in case_mappings:
            mappings_list.append((mapping, mapping))

    sorted_mappings = sorted(mappings_list, key=lambda mapping: mapping[0])

    for mapping in sorted_mappings:
        if mapping[0] <= 0x7f:
            continue # ascii is special cased above.

        if mapping[0] in turkishMapping:
            out.write("case 0x%x: return mode == CaseFoldMode::kTurkish ? 0x%x : 0x%x;\n"
                      % (mapping[0], turkishMapping[mapping[0]], mapping[1]))
        else:
            out.write("case 0x%x: return 0x%x;\n"%mapping)

    out.write("\
    default: return codepoint;\n    }\n}")

    out.write(closeNamespaces())

if __name__ == "__main__":
    generate(sys.argv[1], sys.argv[2])

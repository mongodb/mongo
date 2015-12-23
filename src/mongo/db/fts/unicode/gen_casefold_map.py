 #!/usr/bin/python
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

    out.write("""char32_t codepointToLower(char32_t codepoint, CaseFoldMode \
mode) { 
    if (mode == CaseFoldMode::kTurkish) {
        if (codepoint == 0x049) {  // I -> ı
            return 0x131;
        } else if (codepoint == 0x130) {  // İ -> i
            return 0x069;
        }
    }

    switch (codepoint) {\n""")

    mappings_list = []

    for mapping in case_mappings:
        mappings_list.append((mapping, case_mappings[mapping]))

    sorted_mappings = sorted(mappings_list, key=lambda mapping: mapping[0])

    for mapping in sorted_mappings:
        out.write("\
    case " + str(hex(mapping[0])) + ": return " + \
        str(hex(mapping[1])) +";\n")

    out.write("\
    default: return codepoint;\n    }\n}")

    out.write(closeNamespaces())

if __name__ == "__main__":
    generate(sys.argv[1], sys.argv[2])

 #!/usr/bin/python
 # -*- coding: utf-8 -*-
import sys

from gen_helper import getCopyrightNotice, openNamespaces, closeNamespaces, \
    include

def generate(unicode_proplist_file, target):
    """Generates a C++ source file that contains a delimiter checking function.

    The delimiter checking function contains a switch statement with cases for 
    every delimiter in the Unicode Character Database with the properties 
    specified in delim_properties.
    """
    out = open(target, "w")

    out.write(getCopyrightNotice())
    out.write(include("mongo/db/fts/unicode/codepoints.h"))
    out.write("\n")
    out.write(openNamespaces())

    delim_codepoints = set()

    proplist_file = open(unicode_proplist_file, 'r')

    delim_properties = ["White_Space", 
                        "Dash", 
                        "Hyphen", 
                        "Quotation_Mark", 
                        "Terminal_Punctuation", 
                        "Pattern_Syntax", 
                        "STerm"]

    for line in proplist_file:
        # Filter out blank lines and lines that start with #
        data = line[:line.find('#')]
        if(data == ""):
            continue

        # Parse the data on the line
        values = data.split("; ")
        assert(len(values) == 2)

        uproperty = values[1].strip()
        if uproperty in delim_properties:
            if len(values[0].split('..')) == 2:
                codepoint_range = values[0].split('..')

                start = int(codepoint_range[0], 16)
                end   = int(codepoint_range[1], 16) + 1

                for i in range(start, end):
                    if i not in delim_codepoints: 
                        delim_codepoints.add(i)
            else:
                if int(values[0], 16) not in delim_codepoints:
                    delim_codepoints.add(int(values[0], 16))

    # As of Unicode 8.0.0, all of the delimiters we used for text index 
    # version 2 are also in the list.

    out.write("""bool codepointIsDelimiter(char32_t codepoint, \
DelimiterListLanguage lang) {
    if (lang == DelimiterListLanguage::kEnglish && codepoint == '\\'') {
        return false;
    }

    // Most characters are latin letters, so filter those out first.
    if (codepoint >= 'A' && codepoint <= 'Z') {
        return false;
    } else if (codepoint >= 'a' && codepoint <= 'z') {
        return false;
    }

    switch (codepoint) {\n""")

    for delim in sorted(delim_codepoints):
        out.write("\
    case " + str(hex(delim)) + ": return true;\n")

    out.write("\
    default: return false;\n    }\n}")

    out.write(closeNamespaces())

if __name__ == "__main__":
    generate(sys.argv[1], sys.argv[2])

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.


def main(header, propFile):
    mappings = {}

    with open(propFile, "r") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("#"):
                parts = line.split("=", 1)
                if len(parts) == 2 and len(parts[0]) > 0:
                    mappings[parts[0].strip()] = parts[1].strip()

    keys = mappings.keys()

    header.write("// This is a generated file. Please do not edit.\n")
    header.write("// Please edit the corresponding .properties file instead.\n")

    entries = [
        '{ "%s", "%s", %d }' % (key, mappings[key], len(mappings[key]))
        for key in sorted(keys)
    ]
    header.write(",\n".join(entries) + "\n")

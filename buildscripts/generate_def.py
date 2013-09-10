#!/usr/bin/python

#    Copyright 2013 10gen Inc.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#

"""Generate a def file exposing all public symbols from .lib library files

Usage:
    python generate_def.py <def file> <library files>
"""

import re
import string
import subprocess
import sys


def main(argv):
    defFilename = argv[1]
    allLibFilenames = argv[2:]
    defFile = open(defFilename, 'w')
    defFile.write("LIBRARY " + re.search(r"\\([a-zA-Z0-9_]+).def", defFilename).group(1) + "\n")
    defFile.write("EXPORTS\n")
    for libFilename in allLibFilenames:
        # Only use symbols from linkermember 1; ignore 2 and 3
        dumpBin = subprocess.Popen(["DUMPBIN", "/LINKERMEMBER:1", libFilename], 
                                   stdout=subprocess.PIPE)
        # advance to the symbol section
        seenPublicSymbols = False
        for line in dumpBin.stdout:
            if seenPublicSymbols == False:
                if "public symbols" not in line:
                    continue
                else:
                    seenPublicSymbols = True
                    continue
            if line.startswith("Archive member name"):
                # New linkermember section; skip ahead to the symbol section
                seenPublicSymbols = False
                continue
            # the symbol section ends at this label
            if line.startswith("  Summary"):
                break
            # leave off the symbol address in the first 10 characters
            defFile.write(line[10:])
            
        dumpBin.wait()
    defFile.close()

if __name__ == '__main__':
    main(sys.argv)

# Copyright 2020 MongoDB Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

import os
import sys


def jsToHeader(target, source):
    outFile = target

    h = [
        '#include "mongo/base/string_data.h"',
        '#include "mongo/scripting/engine.h"',
        "namespace mongo {",
        "namespace JSFiles{",
    ]

    def lineToChars(s):
        return ",".join(str(ord(c)) for c in (s.rstrip() + "\n")) + ","

    for s in source:
        filename = str(s)
        objname = os.path.split(filename)[1].split(".")[0]
        stringname = "_jscode_raw_" + objname

        h.append("constexpr char " + stringname + "[] = {")

        with open(filename, "r") as f:
            for line in f:
                h.append(lineToChars(line))

        h.append("0};")
        # symbols aren't exported w/o this
        h.append("extern const JSFile %s;" % objname)
        h.append(
            'const JSFile %s = { "%s", StringData(%s, sizeof(%s) - 1) };'
            % (objname, filename.replace("\\", "/"), stringname, stringname)
        )

    h.append("} // namespace JSFiles")
    h.append("} // namespace mongo")
    h.append("")

    text = "\n".join(h)

    with open(outFile, "w") as out:
        try:
            out.write(text)
        finally:
            out.close()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Must specify [target] [source] ")
        sys.exit(1)

    jsToHeader(sys.argv[1], sys.argv[2:])

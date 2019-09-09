# Trim all trailing whitespace in front of goto labels.
# This is a workaround for a Clang Format limitation where goto labels are
# automatically indented according to nesting.
import re, sys

# 1. Zero or more whitespace characters.
# 2. One or more lowercase ASCII characters.
# 3. Colon character.
p = re.compile('^\s*[a-z]+:$')
for line in sys.stdin:
    m = p.search(line)
    if m is not None:
        sline = line.lstrip()
        # The "default" tag in a switch statement looks identical so we need
        # to filter these out here.
        if not sline.startswith('default'):
            line = sline
    sys.stdout.write(line)

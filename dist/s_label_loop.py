# Mark outer loop boundaries with {@ and }@ .  Nested loops are not marked.
# Each input line is the content of a C function.
import re, sys

p = re.compile('((for |while |_FOREACH|FOREACH_BEGIN)\([^{)]*\)|do) {')
for line in sys.stdin:
    matched = 0
    m = p.search(line)
    while m != None:
        matched = 1
        pos = m.end()
        out = line[:pos] + "@"
        level = 1
        length = len(line)
        while level > 0 and pos < length:
            c = line[pos:pos+1]
            pos += 1
            out += c
            if c == "}":
                level -= 1
            elif c == "{":
                level += 1
        out += "@"
        sys.stdout.write(out)
        line = line[pos:]
        m = p.search(line)
    if matched != 0:
        sys.stdout.write(line)

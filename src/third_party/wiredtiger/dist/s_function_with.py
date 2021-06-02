# Examine WT_WITH_* macros and complain if an early exit out of the
# macro is done.  These macros potentially restore state after they execute
# an expression, and an early exit, like a return/goto means the state
# will not be restored.
#
# Files appear verbatim, separated by a recognizable pattern.
import re, sys

def get_indent(line):
    return len(line) - len(line.lstrip())

filepat = re.compile('===(.*)===')
withpat = re.compile('WT_WITH_')
exitpat = re.compile('(WT_ERR|WT_RET|return\(|goto )')
filename = ''
linenum = 0
with_indent = -1
with_linenum = -1
with_line = None
for line in sys.stdin:
    m = filepat.search(line)
    if m != None:
        filename = m.group(1)
        linenum = 0
        with_indent = -1
        with_linenum = -1
        with_line = None
    linenum += 1

    # Are we within a WT_WITH?
    if with_indent >= 0:
        cur_indent = get_indent(line)
        # Have we "broken out" of the WT_WITH_.... macro?
        # Our enforced formatting ensures this happens when the indent finishes.
        if cur_indent <= with_indent:
            # Yes, broken out
            with_indent = -1
            with_linenum = -1
            with_line = None
        elif exitpat.search(line):
            print('{}: line {}: early exit: {}\n  from: {}: line {}: {}'.format(
                filename, linenum, line.strip(), filename, with_linenum,
                with_line.strip()))
    else:
        m = withpat.search(line)
        if m:
            with_indent = get_indent(line)
            with_linenum = linenum
            with_line = line

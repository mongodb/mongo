#!/usr/bin/python -B
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.


""" Usage: align_stack_comment.py FILE

    This script aligns the stack transition comment in BytecodeEmitter and
    its helper classes.

    The stack transition comment looks like the following:
      //        [stack] VAL1 VAL2 VAL3
"""

import re
import sys

# The column index of '[' of '[stack]'
ALIGNMENT_COLUMN = 20

# The maximum column for comment
MAX_CHARS_PER_LINE = 80

stack_comment_pat = re.compile("^( *//) *(\[stack\].*)$")


def align_stack_comment(path):
    lines = []
    changed = False

    with open(path) as f:
        max_head_len = 0
        max_comment_len = 0

        line_num = 0

        for line in f:
            line_num += 1
            # Python includes \n in lines.
            line = line.rstrip("\n")

            m = stack_comment_pat.search(line)
            if m:
                head = m.group(1) + " "
                head_len = len(head)
                comment = m.group(2)
                comment_len = len(comment)

                if head_len > ALIGNMENT_COLUMN:
                    print(
                        "Warning: line {} overflows from alignment column {}: {}".format(
                            line_num, ALIGNMENT_COLUMN, head_len
                        ),
                        file=sys.stderr,
                    )

                line_len = max(head_len, ALIGNMENT_COLUMN) + comment_len
                if line_len > MAX_CHARS_PER_LINE:
                    print(
                        "Warning: line {} overflows from {} chars: {}".format(
                            line_num, MAX_CHARS_PER_LINE, line_len
                        ),
                        file=sys.stderr,
                    )

                max_head_len = max(max_head_len, head_len)
                max_comment_len = max(max_comment_len, comment_len)

                spaces = max(ALIGNMENT_COLUMN - head_len, 0)
                formatted = head + " " * spaces + comment

                if formatted != line:
                    changed = True

                lines.append(formatted)
            else:
                lines.append(line)

        print(
            "Info: Minimum column number for [stack]: {}".format(max_head_len),
            file=sys.stderr,
        )
        print(
            "Info: Alignment column number for [stack]: {}".format(ALIGNMENT_COLUMN),
            file=sys.stderr,
        )
        print(
            "Info: Max length of stack transition comments: {}".format(max_comment_len),
            file=sys.stderr,
        )

    if changed:
        with open(path, "w") as f:
            for line in lines:
                print(line, file=f)
    else:
        print("No change.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: align_stack_comment.py FILE", file=sys.stderr)
        sys.exit(1)

    for path in sys.argv[1:]:
        print(path)
        align_stack_comment(path)

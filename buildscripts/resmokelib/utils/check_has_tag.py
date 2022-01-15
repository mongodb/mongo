#!/usr/bin/env python3
"""CLI interface for jscomment."""

import sys
import jscomment

try:
    if len(sys.argv) != 3:
        print(
            'This program checks if a javascript test has specific tag (e.g.: @tag=[name] in the comment)'
        )
        print('It returns result via exit code:')
        print('   0 if script has specified tag')
        print('   1 if script does not have specified tag')
        print('   2 if script was invoked incorrectly')
        print('   3 if any error happened during check')
        print('Usage:')
        print(' check_has_tag.py <jsfile> <tag>')
        sys.exit(2)
    else:
        tags = jscomment.get_tags(sys.argv[1])
        print(sys.argv[1], "has tags:", tags)
        sys.exit(0 if sys.argv[2] in tags else 1)
except Exception as err:  # pylint: disable=W0703
    print(err)
    sys.exit(3)

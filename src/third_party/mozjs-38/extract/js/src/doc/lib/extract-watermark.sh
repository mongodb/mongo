#!/usr/bin/env bash

# Extract the js/src/doc watermark from standard input, assumed to be the
# text of a page formatted for publication on MDN.
#
# We can apply this to both the output of the formatter, and to pages
# retrieved from MDN, to see if anything needs to be updated.
#
# Usage:
#
#     extract-watermark.sh
#
# For example:
#
#   $ curl --silent 'https://developer.mozilla.org/en-US/docs/Tools/Debugger-API' | ./doc/lib/extract-watermark.sh
#   sha256:168224ee2d58521b7c8368eddf4ac4fa53a897fa803ae484007af4e61a00ddcd
#   $

set -eu

# Since $(foo) trims any final newline in foo's output, this 'echo' ensures
# that our output is terminated by a newline, whether or not curl | sed's
# is.
echo $(sed -n -e "s|.*<dd id=.watermark.>\([^<]*\)</dd>.*|\1|p")

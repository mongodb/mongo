#!/bin/sh
#
# Doxygen markdown doesn't support triple-backticks like github does.
# Convert all of those to space-prefixed blocks instead.
#
awk '/```/ { prefix=!prefix; print ""; next; } { if (prefix) { printf "    "; } print $0; } ' "$@"

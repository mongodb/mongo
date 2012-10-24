#!/bin/sh
#
# Copyright (c) 2011, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# ---
# Author: csilvers@google.com (Craig Silverstein)

if [ -z "$1" ]; then
  echo "USAGE: $0 <unittest exe>"
  exit 1
fi
BINARY="$1"

# Make sure the binary exists...
if ! "$BINARY" >/dev/null 2>/dev/null
then
  echo "Cannot run binary $BINARY"
  exit 1
fi

# Make sure the --help output doesn't print the stripped text.
if "$BINARY" --help | grep "This text should be stripped out" >/dev/null 2>&1
then
  echo "Text not stripped from --help like it should be: $BINARY"
  exit 1
fi

# Make sure the stripped text isn't in the binary at all.
if strings --help >/dev/null 2>&1    # make sure the binary exists
then
  # Unfortunately, for us, libtool can replace executables with a
  # shell script that does some work before calling the 'real'
  # executable under a different name.  We need the 'real'
  # executable name to run 'strings' on it, so we construct this
  # binary to print the real name (argv[0]) on stdout when run.
  REAL_BINARY=`"$BINARY"`
  # On cygwin, we may need to add a '.exe' extension by hand.
  [ -f "$REAL_BINARY.exe" ] && REAL_BINARY="$REAL_BINARY.exe"
  if strings "$REAL_BINARY" | grep "This text should be stripped" >/dev/null 2>&1
  then
    echo "Text not stripped from binary like it should be: $BINARY"
    exit 1
  fi

  # Let's also do a sanity check to make sure strings is working properly
  if ! strings "$REAL_BINARY" | grep "Usage message" >/dev/null 2>&1
  then
    echo "Usage text not found in binary like it should be: $BINARY"
    exit 1
  fi
fi

echo "PASS"

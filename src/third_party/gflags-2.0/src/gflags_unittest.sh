#!/bin/bash

# Copyright (c) 2006, Google Inc.
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

# ---
# Author: Craig Silverstein
#
# Just tries to run the gflags_unittest with various flags
# defined in gflags.cc, and make sure they give the
# appropriate exit status and appropriate error message.

if [ -z "$1" ]; then
  echo "USAGE: $0 <unittest exe> [top_srcdir] [tmpdir]"
  exit 1
fi 
EXE="$1"
SRCDIR="${2:-./}"
TMPDIR="${3:-/tmp/gflags}"
EXE2="${EXE}2"    # eg, gflags_unittest2
EXE3="${EXE}3"    # eg, gflags_unittest3

# $1: executable
# $2: line-number $3: expected return code.  $4: substring of expected output.
# $5: a substring you *don't* expect to find in the output.  $6+ flags
ExpectExe() {
  local executable="$1"
  shift
  local line_number="$1"
  shift
  local expected_rc="$1"
  shift
  local expected_output="$1"
  shift
  local unexpected_output="$1"
  shift

    # We always add --srcdir because it's needed for correctness
    "$executable" --srcdir="$SRCDIR" "$@" > "$TMPDIR/test.$line_number" 2>&1

  local actual_rc=$?
  if [ $actual_rc != $expected_rc ]; then
    echo "Test on line $line_number failed:" \
         "expected rc $expected_rc, got $actual_rc"
    exit 1;
  fi
  if [ -n "$expected_output" ] &&
     ! fgrep -e "$expected_output" "$TMPDIR/test.$line_number" >/dev/null; then
    echo "Test on line $line_number failed:" \
         "did not find expected substring '$expected_output'"
    exit 1;
  fi
  if [ -n "$unexpected_output" ] &&
     fgrep -e "$unexpected_output" "$TMPDIR/test.$line_number" >/dev/null; then
    echo "Test line $line_number failed:" \
         "found unexpected substring '$unexpected_output'"
    exit 1;
  fi
}

# $1: line-number $2: expected return code.  $3: substring of expected output.
# $4: a substring you *don't* expect to find in the output.  $5+ flags
Expect() {
  ExpectExe "$EXE" "$@"
}

rm -rf "$TMPDIR"
mkdir "$TMPDIR" || exit 2

# Create a few flagfiles we can use later
echo "--version" > "$TMPDIR/flagfile.1"
echo "--foo=bar" > "$TMPDIR/flagfile.2"
echo "--nounused_bool" >> "$TMPDIR/flagfile.2"
echo "--flagfile=$TMPDIR/flagfile.2" > "$TMPDIR/flagfile.3"

# Set a few environment variables (useful for --tryfromenv)
export FLAGS_undefok=foo,bar
export FLAGS_weirdo=
export FLAGS_version=true
export FLAGS_help=false

# First, just make sure the unittest works as-is
Expect $LINENO 0 "PASS" ""

# --help should show all flags, including flags from gflags_reporting
Expect $LINENO 1 "/gflags_reporting.cc" "" --help

# Make sure that --help prints even very long helpstrings.
Expect $LINENO 1 "end of a long helpstring" "" --help

# Make sure --help reflects flag changes made before flag-parsing
Expect $LINENO 1 \
     "-changed_bool1 (changed) type: bool default: true" "" --help
Expect $LINENO 1 \
     "-changed_bool2 (changed) type: bool default: false currently: true" "" \
     --help
# And on the command-line, too
Expect $LINENO 1 \
     "-changeable_string_var () type: string default: \"1\" currently: \"2\"" \
     "" --changeable_string_var 2 --help

# --nohelp and --help=false should be as if we didn't say anything
Expect $LINENO 0 "PASS" "" --nohelp
Expect $LINENO 0 "PASS" "" --help=false

# --helpfull is the same as help
Expect $LINENO 1 "/gflags_reporting.cc" "" -helpfull

# --helpshort should show only flags from the unittest itself
Expect $LINENO 1 "/gflags_unittest.cc" \
       "/gflags_reporting.cc" --helpshort

# --helpshort should show the tldflag we created in the unittest dir
Expect $LINENO 1 "tldflag1" "/google.cc" --helpshort
Expect $LINENO 1 "tldflag2" "/google.cc" --helpshort

# --helpshort should work if the main source file is suffixed with [_-]main
ExpectExe "$EXE2" $LINENO 1 "/gflags_unittest-main.cc" \
          "/gflags_reporting.cc" --helpshort
ExpectExe "$EXE3" $LINENO 1 "/gflags_unittest_main.cc" \
          "/gflags_reporting.cc" --helpshort

# --helpon needs an argument
Expect $LINENO 1 \
     "'--helpon' is missing its argument; flag description: show help on" \
     "" --helpon

# --helpon argument indicates what file we'll show args from
Expect $LINENO 1 "/gflags.cc" "/gflags_unittest.cc" \
  --helpon=gflags

# another way of specifying the argument
Expect $LINENO 1 "/gflags.cc" "/gflags_unittest.cc" \
       --helpon gflags

# test another argument
Expect $LINENO 1 "/gflags_unittest.cc" "/gflags.cc" \
  --helpon=gflags_unittest

# helpmatch is like helpon but takes substrings
Expect $LINENO 1 "/gflags_reporting.cc" \
       "/gflags_unittest.cc" -helpmatch reporting
Expect $LINENO 1 "/gflags_unittest.cc" \
       "/gflags.cc" -helpmatch=unittest

# if no flags are found with helpmatch or helpon, suggest --help
Expect $LINENO 1 "No modules matched" "/gflags_unittest.cc" \
  -helpmatch=nosuchsubstring
Expect $LINENO 1 "No modules matched" "/gflags_unittest.cc" \
  -helpon=nosuchmodule

# helppackage shows all the flags in the same dir as this unittest
# --help should show all flags, including flags from google.cc
Expect $LINENO 1 "/gflags_reporting.cc" "" --helppackage

# xml!
Expect $LINENO 1 "/gflags_unittest.cc</file>" \
  "/gflags_unittest.cc:" --helpxml

# just print the version info and exit
Expect $LINENO 0 "gflags_unittest" "gflags_unittest.cc" --version
Expect $LINENO 0 "version test_version" "gflags_unittest.cc" --version

# --undefok is a fun flag...
Expect $LINENO 1 "unknown command line flag 'foo'" "" --undefok= --foo --unused_bool
Expect $LINENO 0 "PASS" "" --undefok=foo --foo --unused_bool
# If you say foo is ok to be undefined, we'll accept --nofoo as well
Expect $LINENO 0 "PASS" "" --undefok=foo --nofoo --unused_bool
# It's ok if the foo is in the middle
Expect $LINENO 0 "PASS" "" --undefok=fee,fi,foo,fum --foo --unused_bool
# But the spelling has to be just right...
Expect $LINENO 1 "unknown command line flag 'foo'" "" --undefok=fo --foo --unused_bool
Expect $LINENO 1 "unknown command line flag 'foo'" "" --undefok=foot --foo --unused_bool

# See if we can successfully load our flags from the flagfile
Expect $LINENO 0 "gflags_unittest" "gflags_unittest.cc" \
  --flagfile="$TMPDIR/flagfile.1"
Expect $LINENO 0 "PASS" "" --flagfile="$TMPDIR/flagfile.2"
Expect $LINENO 0 "PASS" "" --flagfile="$TMPDIR/flagfile.3"

# Also try to load flags from the environment
Expect $LINENO 0 "gflags_unittest" "gflags_unittest.cc" \
  --fromenv=version
Expect $LINENO 0 "gflags_unittest" "gflags_unittest.cc" \
  --tryfromenv=version
Expect $LINENO 0 "PASS" "" --fromenv=help
Expect $LINENO 0 "PASS" "" --tryfromenv=help
Expect $LINENO 1 "helpfull not found in environment" "" --fromenv=helpfull
Expect $LINENO 0 "PASS" "" --tryfromenv=helpfull
Expect $LINENO 0 "PASS" "" --tryfromenv=undefok --foo
Expect $LINENO 1 "unknown command line flag" "" --tryfromenv=weirdo
Expect $LINENO 0 "gflags_unittest" "gflags_unittest.cc" \
  --tryfromenv=test_bool,version,unused_bool
Expect $LINENO 1 "not found in environment" "" --fromenv=test_bool
Expect $LINENO 1 "unknown command line flag" "" --fromenv=test_bool,ok
# Here, the --version overrides the fromenv
Expect $LINENO 0 "gflags_unittest" "gflags_unittest.cc" \
  --fromenv=test_bool,version,ok

# Make sure -- by itself stops argv processing
Expect $LINENO 0 "PASS" "" -- --help


# And we should die if the flag value doesn't pass the validator
Expect $LINENO 1 "ERROR: failed validation of new value 'true' for flag 'always_fail'" "" --always_fail

# TODO(user) And if locking in validators fails.
# Expect $LINENO 0 "PASS" "" --deadlock_if_cant_lock

echo "PASS"
exit 0

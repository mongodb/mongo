#!/bin/bash

# Copyright (c) 2008, Google Inc.
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
# Author: Dave Nicponski
#
# This script is invoked by bash in response to a matching compspec.  When
# this happens, bash calls this script using the command shown in the -C
# block of the complete entry, but also appends 3 arguments.  They are:
#   - The command being used for completion
#   - The word being completed
#   - The word preceding the completion word.
#
# Here's an example of how you might use this script:
# $ complete -o bashdefault -o default -o nospace -C                         \
#   '/usr/local/bin/gflags_completions.sh --tab_completion_columns $COLUMNS' \
#   time  env  binary_name  another_binary  [...]

# completion_word_index gets the index of the (N-1)th argument for
# this command line.  completion_word gets the actual argument from
# this command line at the (N-1)th position
completion_word_index="$(($# - 1))"
completion_word="${!completion_word_index}"

# TODO(user): Replace this once gflags_completions.cc has
# a bool parameter indicating unambiguously to hijack the process for
# completion purposes.
if [ -z "$completion_word" ]; then
  # Until an empty value for the completion word stops being misunderstood
  # by binaries, don't actually execute the binary or the process
  # won't be hijacked!
  exit 0
fi

# binary_index gets the index of the command being completed (which bash
# places in the (N-2)nd position.  binary gets the actual command from
# this command line at that (N-2)nd position
binary_index="$(($# - 2))"
binary="${!binary_index}"

# For completions to be universal, we may have setup the compspec to
# trigger on 'harmless pass-through' commands, like 'time' or 'env'.
# If the command being completed is one of those two, we'll need to
# identify the actual command being executed.  To do this, we need
# the actual command line that the <TAB> was pressed on.  Bash helpfully
# places this in the $COMP_LINE variable.
if [ "$binary" == "time" ] || [ "$binary" == "env" ]; then
  # we'll assume that the first 'argument' is actually the
  # binary


  # TODO(user): This is not perfect - the 'env' command, for instance,
  #   is allowed to have options between the 'env' and 'the command to
  #   be executed'.  For example, consider:
  # $ env FOO="bar"  bin/do_something  --help<TAB>
  # In this case, we'll mistake the FOO="bar" portion as the binary.
  #   Perhaps we should continuing consuming leading words until we
  #   either run out of words, or find a word that is a valid file
  #   marked as executable.  I can't think of any reason this wouldn't
  #   work.

  # Break up the 'original command line' (not this script's command line,
  # rather the one the <TAB> was pressed on) and find the second word.
  parts=( ${COMP_LINE} )
  binary=${parts[1]}
fi

# Build the command line to use for completion.  Basically it involves
# passing through all the arguments given to this script (except the 3
# that bash added), and appending a '--tab_completion_word "WORD"' to
# the arguments.
params=""
for ((i=1; i<=$(($# - 3)); ++i)); do 
  params="$params \"${!i}\"";
done
params="$params --tab_completion_word \"$completion_word\""

# TODO(user): Perhaps stash the output in a temporary file somewhere
# in /tmp, and only cat it to stdout if the command returned a success
# code, to prevent false positives

# If we think we have a reasonable command to execute, then execute it
# and hope for the best.
candidate=$(type -p "$binary")
if [ ! -z "$candidate" ]; then
  eval "$candidate 2>/dev/null $params"
elif [ -f "$binary" ] && [ -x "$binary" ]; then
  eval "$binary 2>/dev/null $params"
fi

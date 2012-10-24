// Copyright (c) 2011, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// ---
// Author: csilvers@google.com (Craig Silverstein)
//
// A simple program that uses STRIP_FLAG_HELP.  We'll have a shell
// script that runs 'strings' over this program and makes sure
// that the help string is not in there.

#include "config_for_unittests.h"
#define STRIP_FLAG_HELP 1
#include <gflags/gflags.h>

#include <stdio.h>

using GOOGLE_NAMESPACE::SetUsageMessage;
using GOOGLE_NAMESPACE::ParseCommandLineFlags;


DEFINE_bool(test, true, "This text should be stripped out");

int main(int argc, char** argv) {
  SetUsageMessage("Usage message");
  ParseCommandLineFlags(&argc, &argv, false);

  // Unfortunately, for us, libtool can replace executables with a shell
  // script that does some work before calling the 'real' executable
  // under a different name.  We need the 'real' executable name to run
  // 'strings' on it, so we construct this binary to print the real
  // name (argv[0]) on stdout when run.
  puts(argv[0]);

  return 0;
}

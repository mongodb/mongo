// Copyright (c) 2009, Google Inc.
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

// ---
//
// A negative comiple test for gflags.

#include <gflags/gflags.h>

#if defined(TEST_SWAPPED_ARGS)

DEFINE_bool(some_bool_flag,
            "the default value should go here, not the description",
            false);


#elif defined(TEST_INT_INSTEAD_OF_BOOL)

DEFINE_bool(some_bool_flag_2,
            0,
            "should have been an int32 flag but mistakenly used bool instead");

#elif defined(TEST_BOOL_IN_QUOTES)


DEFINE_bool(some_bool_flag_3,
            "false",
            "false in in quotes, which is wrong");

#elif defined(TEST_SANITY)

DEFINE_bool(some_bool_flag_4,
            true,
            "this is the correct usage of DEFINE_bool");

#elif defined(TEST_DEFINE_STRING_WITH_0)

DEFINE_string(some_string_flag,
              0,
              "Trying to construct a string by passing 0 would cause a crash.");

#endif

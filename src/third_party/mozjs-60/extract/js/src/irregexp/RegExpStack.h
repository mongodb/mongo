/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */

// Copyright 2009 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
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

#ifndef V8_REGEXP_STACK_H_
#define V8_REGEXP_STACK_H_

#include "jspubtd.h"
#include "js/Utility.h"

namespace js {
namespace irregexp {

class RegExpStack;

// Maintains a per-thread stack area that can be used by irregexp
// implementation for its backtracking stack.
//
// Since there is only one stack area, the Irregexp implementation is not
// re-entrant. I.e., no regular expressions may be executed in the same thread
// during a preempted Irregexp execution.
class RegExpStackScope
{
  public:
    // Create and delete an instance to control the life-time of a growing stack.

    // Initializes the stack memory area if necessary.
    explicit RegExpStackScope(JSContext* cx);

    // Releases the stack if it has grown.
    ~RegExpStackScope();

  private:
    RegExpStack* regexp_stack;
};

class RegExpStack
{
  public:
    // Number of allocated locations on the stack above the limit.
    // No sequence of pushes must be longer that this without doing a stack-limit
    // check.
    static const int kStackLimitSlack = 32;

    RegExpStack();
    ~RegExpStack();
    bool init();

    // Resets the buffer if it has grown beyond the default/minimum size.
    void reset();

    // Attempts to grow the stack by at least kStackLimitSlack entries.
    bool grow();

    // Address of allocated memory.
    static size_t offsetOfBase() { return offsetof(RegExpStack, base_); }
    static size_t offsetOfLimit() { return offsetof(RegExpStack, limit_); }

    void* base() { return base_; }
    void* limit() { return limit_; }

  private:
    // Artificial limit used when no memory has been allocated.
    static const uintptr_t kMemoryTop = static_cast<uintptr_t>(-1);

    // Minimal size of allocated stack area, in bytes.
    static const size_t kMinimumStackSize = 512;

    // Maximal size of allocated stack area, in bytes.
    static const size_t kMaximumStackSize = 64 * 1024 * 1024;

    // If size > 0 then base must be non-nullptr.
    void* base_;

    // Length in bytes of base.
    size_t size;

    // If the stack pointer gets above the limit, we should react and
    // either grow the stack or report an out-of-stack exception.
    // There is only a limited number of locations above the stack limit,
    // so users of the stack should check the stack limit during any
    // sequence of pushes longer than this.
    void* limit_;

    void updateLimit() {
        MOZ_ASSERT(size >= kStackLimitSlack * sizeof(void*));
        limit_ = static_cast<uint8_t*>(base()) + size - (kStackLimitSlack * sizeof(void*));
    }
};

bool
GrowBacktrackStack(JSRuntime* rt);

}}  // namespace js::irregexp

#endif  // V8_REGEXP_STACK_H_

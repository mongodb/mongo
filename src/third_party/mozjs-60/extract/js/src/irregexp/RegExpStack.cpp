/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */

// Copyright 2012 the V8 project authors. All rights reserved.
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

#include "irregexp/RegExpStack.h"

#include "vm/JSContext.h"

using namespace js;
using namespace js::irregexp;

RegExpStackScope::RegExpStackScope(JSContext* cx)
  : regexp_stack(&cx->regexpStack.ref())
{}

RegExpStackScope::~RegExpStackScope()
{
    regexp_stack->reset();
}

bool
irregexp::GrowBacktrackStack(JSRuntime* rt)
{
    AutoUnsafeCallWithABI unsafe;
    return TlsContext.get()->regexpStack.ref().grow();
}

RegExpStack::RegExpStack()
  : base_(nullptr), size(0), limit_(nullptr)
{}

RegExpStack::~RegExpStack()
{
    js_free(base_);
}

bool
RegExpStack::init()
{
    base_ = js_malloc(kMinimumStackSize);
    if (!base_)
        return false;

    size = kMinimumStackSize;
    updateLimit();
    return true;
}

void
RegExpStack::reset()
{
    MOZ_ASSERT(size >= kMinimumStackSize);

    if (size != kMinimumStackSize) {
        void* newBase = js_realloc(base_, kMinimumStackSize);
        if (!newBase)
            return;

        base_ = newBase;
        size = kMinimumStackSize;
        updateLimit();
    }
}

bool
RegExpStack::grow()
{
    size_t newSize = size * 2;
    if (newSize > kMaximumStackSize)
        return false;

    void* newBase = js_realloc(base_, newSize);
    if (!newBase)
        return false;

    base_ = newBase;
    size = newSize;
    updateLimit();

    return true;
}

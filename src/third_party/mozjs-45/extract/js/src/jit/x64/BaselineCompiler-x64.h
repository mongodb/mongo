/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_BaselineCompiler_x64_h
#define jit_x64_BaselineCompiler_x64_h

#include "jit/x86-shared/BaselineCompiler-x86-shared.h"

namespace js {
namespace jit {

class BaselineCompilerX64 : public BaselineCompilerX86Shared
{
  protected:
    BaselineCompilerX64(JSContext* cx, TempAllocator& alloc, JSScript* script);
};

typedef BaselineCompilerX64 BaselineCompilerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x64_BaselineCompiler_x64_h */

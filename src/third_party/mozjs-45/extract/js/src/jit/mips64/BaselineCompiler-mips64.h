/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_BaselineCompiler_mips64_h
#define jit_mips64_BaselineCompiler_mips64_h

#include "jit/mips-shared/BaselineCompiler-mips-shared.h"

namespace js {
namespace jit {

class BaselineCompilerMIPS64 : public BaselineCompilerMIPSShared
{
  protected:
    BaselineCompilerMIPS64(JSContext* cx, TempAllocator& alloc, JSScript* script);
};

typedef BaselineCompilerMIPS64 BaselineCompilerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_mips64_BaselineCompiler_mips64_h */

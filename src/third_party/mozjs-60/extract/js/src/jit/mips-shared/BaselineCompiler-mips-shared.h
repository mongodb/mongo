/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_BaselineCompiler_mips_shared_h
#define jit_mips_shared_BaselineCompiler_mips_shared_h

#include "jit/shared/BaselineCompiler-shared.h"

namespace js {
namespace jit {

class BaselineCompilerMIPSShared : public BaselineCompilerShared
{
  protected:
    BaselineCompilerMIPSShared(JSContext* cx, TempAllocator& alloc, JSScript* script);
};

} // namespace jit
} // namespace js

#endif /* jit_mips_shared_BaselineCompiler_mips_shared_h */

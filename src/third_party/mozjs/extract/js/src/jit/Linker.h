/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Linker_h
#define jit_Linker_h

#include "jit/ExecutableAllocator.h"
#include "jit/IonCode.h"
#include "jit/JitCompartment.h"
#include "jit/MacroAssembler.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"

namespace js {
namespace jit {

class Linker
{
    MacroAssembler& masm;
    mozilla::Maybe<AutoWritableJitCode> awjc;

    JitCode* fail(JSContext* cx) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

  public:
    // Construct a linker with a rooted macro assembler.
    explicit Linker(MacroAssembler& masm)
      : masm(masm)
    {
        MOZ_ASSERT(masm.isRooted());
        masm.finish();
    }

    // If the macro assembler isn't rooted then care must be taken as it often
    // contains GC pointers.
    Linker(MacroAssembler& masm, JS::AutoRequireNoGC& nogc)
      : masm(masm)
    {
        masm.finish();
    }

    // Create a new JitCode object and populate it with the contents of the
    // macro assember buffer.
    //
    // This method cannot GC. Errors are reported to the context.
    JitCode* newCode(JSContext* cx, CodeKind kind, bool hasPatchableBackedges = false);
};

} // namespace jit
} // namespace js

#endif /* jit_Linker_h */

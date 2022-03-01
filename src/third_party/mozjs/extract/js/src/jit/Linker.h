/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Linker_h
#define jit_Linker_h

#include "mozilla/Maybe.h"

#include <stdint.h>

#include "jstypes.h"

#include "jit/AutoWritableJitCode.h"
#include "jit/MacroAssembler.h"
#include "vm/Runtime.h"

struct JS_PUBLIC_API JSContext;

namespace js {
namespace jit {

class JitCode;

enum class CodeKind : uint8_t;

class Linker {
  MacroAssembler& masm;
  mozilla::Maybe<AutoWritableJitCodeFallible> awjcf;

  JitCode* fail(JSContext* cx) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

 public:
  // Construct a linker with a rooted macro assembler.
  explicit Linker(MacroAssembler& masm) : masm(masm) { masm.finish(); }

  // Create a new JitCode object and populate it with the contents of the
  // macro assember buffer.
  //
  // This method cannot GC. Errors are reported to the context.
  JitCode* newCode(JSContext* cx, CodeKind kind);
};

}  // namespace jit
}  // namespace js

#endif /* jit_Linker_h */

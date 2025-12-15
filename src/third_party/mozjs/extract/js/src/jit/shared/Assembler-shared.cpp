/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/shared/Assembler-shared.h"

#include "jit/JitSpewer.h"
#include "vm/NativeObject.h"
#include "wasm/WasmMemory.h"

namespace js {

namespace wasm {

#ifdef DEBUG
void MemoryAccessDesc::assertOffsetInGuardPages() const {
  MOZ_ASSERT(offset_ < (uint64_t)GetMaxOffsetGuardLimit(hugeMemory_));
}
#endif

}  // namespace wasm

namespace jit {

void BaseObjectElementIndex::staticAssertions() {
  NativeObject::elementsSizeMustNotOverflow();
}

void BaseObjectSlotIndex::staticAssertions() {
  NativeObject::slotsSizeMustNotOverflow();
}

AssemblerShared::~AssemblerShared() {
#ifdef DEBUG
  while (hasCreator()) {
    popCreator();
  }
#endif
}

#ifdef DEBUG
void AssemblerShared::pushCreator(const char* who) {
  (void)creators_.append(who);
  JitSpewStart(JitSpew_Codegen, "# BEGIN creators: ");
  bool first = true;
  for (const char* str : creators_) {
    JitSpewCont(JitSpew_Codegen, "%s%s", first ? "" : "/", str);
    first = false;
  }
  JitSpewCont(JitSpew_Codegen, "\n");
}

void AssemblerShared::popCreator() {
  JitSpewStart(JitSpew_Codegen, "# END   creators: ");
  bool first = true;
  for (const char* str : creators_) {
    JitSpewCont(JitSpew_Codegen, "%s%s", first ? "" : "/", str);
    first = false;
  }
  JitSpewCont(JitSpew_Codegen, "\n");
  if (creators_.empty()) {
    JitSpew(JitSpew_Codegen, " ");
  }
  MOZ_ASSERT(!creators_.empty());
  creators_.popBack();
}

bool AssemblerShared::hasCreator() const {
  // If you get failures of assertions of the form `MOZ_ASSERT(hasCreator())`,
  // what this means is that a `MacroAssembler` (or, really, anything that
  // inherits from `js::jit::AssemblerShared`) has emitted code or data from a
  // place, in the SM C++ hierarchy, that is not nested within an
  // `AutoCreatedBy` RAII scope.  Consequently the emitted instructions/data
  // won't have any owner that is identifiable in the `IONFLAGS=codegen`
  // output.
  //
  // Fixing this is easy: work back up the crash stack and decide on a place
  // to put an `AutoCreatedBy` call.  A bit of grepping for `AutoCreatedBy`
  // should make it obvious what to do.  If in doubt, add `AutoCreatedBy`
  // calls liberally; "extra" ones are harmless.
  return !creators_.empty();
}
#endif

}  // namespace jit

}  // namespace js

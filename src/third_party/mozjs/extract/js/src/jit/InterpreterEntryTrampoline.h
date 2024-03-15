/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_InterpreterEntryTrampoline_h
#define jit_InterpreterEntryTrampoline_h

#include "gc/Barrier.h"
#include "gc/Tracer.h"
#include "jit/JitCode.h"
#include "js/AllocPolicy.h"
#include "js/HashTable.h"
#include "js/RootingAPI.h"

namespace js {

void ClearInterpreterEntryMap(JSRuntime* runtime);

namespace jit {

/*
 *  The EntryTrampolineMap is used to cache the trampoline code for
 *  each script as they are created.  These trampolines are created
 *  only under --emit-interpreter-entry and are used to identify which
 *  script is being interpeted when profiling with external profilers
 *  such as perf.
 *
 *  The map owns the JitCode objects that are created for each script,
 *  and keeps them alive at least as long as the script associated
 *  with it in case we need to re-enter the trampoline again.
 *
 *  As each script is finalized, the entry is manually removed from
 *  the table in BaseScript::finalize which will also release the
 *  trampoline code associated with it.
 *
 *  During a moving GC, the table is rekeyed in case any scripts
 *  have relocated.
 */

class EntryTrampoline {
  HeapPtr<JitCode*> entryTrampoline_;

 public:
  void trace(JSTracer* trc) {
    TraceNullableEdge(trc, &entryTrampoline_, "interpreter-entry-trampoline");
  }

  explicit EntryTrampoline(JSContext* cx, JitCode* code) {
    MOZ_ASSERT(code);
    entryTrampoline_ = code;
  }

  uint8_t* raw() {
    MOZ_ASSERT(entryTrampoline_, "Empty trampoline code.");
    return entryTrampoline_->raw();
  }

#ifdef JSGC_HASH_TABLE_CHECKS
  void checkTrampolineAfterMovingGC();
#endif
};

using JSScriptToTrampolineMap =
    HashMap<HeapPtr<BaseScript*>, EntryTrampoline,
            DefaultHasher<HeapPtr<BaseScript*>>, SystemAllocPolicy>;
class EntryTrampolineMap : public JSScriptToTrampolineMap {
 public:
  void traceTrampolineCode(JSTracer* trc);
  void updateScriptsAfterMovingGC(void);
#ifdef JSGC_HASH_TABLE_CHECKS
  void checkScriptsAfterMovingGC();
#endif
};

}  // namespace jit
}  // namespace js
#endif /* jit_InterpreterEntryTrampoline_h */

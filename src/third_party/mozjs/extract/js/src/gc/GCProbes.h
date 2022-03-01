/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCProbes_h
#define gc_GCProbes_h

/*
 * This interface can be used to insert probes for GC related events.
 *
 * The code must be built with JS_GC_PROBES for these probes to be called
 * from JIT code.
 */

#include "gc/Heap.h"

namespace js {
namespace gc {
namespace gcprobes {

inline void Init(gc::GCRuntime* gc) {}
inline void Finish(gc::GCRuntime* gc) {}
inline void NurseryAlloc(gc::Cell* thing, size_t size) {}
inline void NurseryAlloc(gc::Cell* thing, JS::TraceKind kind) {}
inline void TenuredAlloc(gc::Cell* thing, gc::AllocKind kind) {}
inline void CreateObject(JSObject* object) {}
inline void MinorGCStart() {}
inline void PromoteToTenured(gc::Cell* src, gc::Cell* dst) {}
inline void MinorGCEnd() {}
inline void MajorGCStart() {}
inline void TenuredFinalize(gc::Cell* thing) {
}  // May be called off main thread.
inline void MajorGCEnd() {}

}  // namespace gcprobes
}  // namespace gc
}  // namespace js

#endif  // gc_GCProbes_h

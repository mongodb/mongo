/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCTrace_h
#define gc_GCTrace_h

#include "gc/Heap.h"

namespace js {

class ObjectGroup;

namespace gc {

#ifdef JS_GC_TRACE

extern bool InitTrace(GCRuntime& gc);
extern void FinishTrace();
extern bool TraceEnabled();
extern void TraceNurseryAlloc(Cell* thing, size_t size);
extern void TraceTenuredAlloc(Cell* thing, AllocKind kind);
extern void TraceCreateObject(JSObject* object);
extern void TraceMinorGCStart();
extern void TracePromoteToTenured(Cell* src, Cell* dst);
extern void TraceMinorGCEnd();
extern void TraceMajorGCStart();
extern void TraceTenuredFinalize(Cell* thing);
extern void TraceMajorGCEnd();
extern void TraceTypeNewScript(js::ObjectGroup* group);

#else

inline bool InitTrace(GCRuntime& gc) { return true; }
inline void FinishTrace() {}
inline bool TraceEnabled() { return false; }
inline void TraceNurseryAlloc(Cell* thing, size_t size) {}
inline void TraceTenuredAlloc(Cell* thing, AllocKind kind) {}
inline void TraceCreateObject(JSObject* object) {}
inline void TraceMinorGCStart() {}
inline void TracePromoteToTenured(Cell* src, Cell* dst) {}
inline void TraceMinorGCEnd() {}
inline void TraceMajorGCStart() {}
inline void TraceTenuredFinalize(Cell* thing) {}
inline void TraceMajorGCEnd() {}
inline void TraceTypeNewScript(js::ObjectGroup* group) {}

#endif

} /* namespace gc */
} /* namespace js */

#endif

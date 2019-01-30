/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal enum definitions.
 */

#ifndef gc_GCEnum_h
#define gc_GCEnum_h

#include <stdint.h>

namespace js {
namespace gc {

// Mark colors to pass to markIfUnmarked.
enum class MarkColor : uint32_t
{
    Black = 0,
    Gray
};

// The phases of an incremental GC.
#define GCSTATES(D) \
    D(NotActive) \
    D(MarkRoots) \
    D(Mark) \
    D(Sweep) \
    D(Finalize) \
    D(Compact) \
    D(Decommit)
enum class State {
#define MAKE_STATE(name) name,
    GCSTATES(MAKE_STATE)
#undef MAKE_STATE
};

// Reasons we reset an ongoing incremental GC or perform a non-incremental GC.
#define GC_ABORT_REASONS(D) \
    D(None) \
    D(NonIncrementalRequested) \
    D(AbortRequested) \
    D(Unused1) \
    D(IncrementalDisabled) \
    D(ModeChange) \
    D(MallocBytesTrigger) \
    D(GCBytesTrigger) \
    D(ZoneChange) \
    D(CompartmentRevived) \
    D(GrayRootBufferingFailed)
enum class AbortReason {
#define MAKE_REASON(name) name,
    GC_ABORT_REASONS(MAKE_REASON)
#undef MAKE_REASON
};

#define JS_FOR_EACH_ZEAL_MODE(D)       \
    D(RootsChange, 1)                  \
    D(Alloc, 2)                        \
    D(VerifierPre, 4)                  \
    D(GenerationalGC, 7)               \
    D(IncrementalRootsThenFinish, 8)   \
    D(IncrementalMarkAllThenFinish, 9) \
    D(IncrementalMultipleSlices, 10)   \
    D(IncrementalMarkingValidator, 11) \
    D(ElementsBarrier, 12)             \
    D(CheckHashTablesOnMinorGC, 13)    \
    D(Compact, 14)                     \
    D(CheckHeapAfterGC, 15)            \
    D(CheckNursery, 16)                \
    D(IncrementalSweepThenFinish, 17)  \
    D(CheckGrayMarking, 18)

enum class ZealMode {
#define ZEAL_MODE(name, value) name = value,
    JS_FOR_EACH_ZEAL_MODE(ZEAL_MODE)
#undef ZEAL_MODE
    Limit = 18
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_GCEnum_h */

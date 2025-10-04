/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal enum definitions.
 */

#ifndef gc_GCEnum_h
#define gc_GCEnum_h

#include <stdint.h>

#include "js/MemoryFunctions.h"  // JS_FOR_EACH_PUBLIC_MEMORY_USE

namespace js {

// [SMDOC] AllowGC template parameter
//
// AllowGC is a template parameter for functions that support both with and
// without GC operation.
//
// The CanGC variant of the function can trigger a garbage collection, and
// should set a pending exception on failure.
//
// The NoGC variant of the function cannot trigger a garbage collection, and
// should not set any pending exception on failure.  This variant can be called
// in fast paths where the caller has unrooted pointers.  The failure means we
// need to perform GC to allocate an object. The caller can fall back to a slow
// path that roots pointers before calling a CanGC variant of the function,
// without having to clear a pending exception.
enum AllowGC { NoGC = 0, CanGC = 1 };

namespace gc {

// The phases of an incremental GC.
#define GCSTATES(D) \
  D(NotActive)      \
  D(Prepare)        \
  D(MarkRoots)      \
  D(Mark)           \
  D(Sweep)          \
  D(Finalize)       \
  D(Compact)        \
  D(Decommit)       \
  D(Finish)
enum class State {
#define MAKE_STATE(name) name,
  GCSTATES(MAKE_STATE)
#undef MAKE_STATE
};

#define JS_FOR_EACH_ZEAL_MODE(D)         \
  D(RootsChange, 1)                      \
  D(Alloc, 2)                            \
  D(VerifierPre, 4)                      \
  D(YieldBeforeRootMarking, 6)           \
  D(GenerationalGC, 7)                   \
  D(YieldBeforeMarking, 8)               \
  D(YieldBeforeSweeping, 9)              \
  D(IncrementalMultipleSlices, 10)       \
  D(IncrementalMarkingValidator, 11)     \
  D(ElementsBarrier, 12)                 \
  D(CheckHashTablesOnMinorGC, 13)        \
  D(Compact, 14)                         \
  D(CheckHeapAfterGC, 15)                \
  D(YieldBeforeSweepingAtoms, 17)        \
  D(CheckGrayMarking, 18)                \
  D(YieldBeforeSweepingCaches, 19)       \
  D(YieldBeforeSweepingObjects, 21)      \
  D(YieldBeforeSweepingNonObjects, 22)   \
  D(YieldBeforeSweepingPropMapTrees, 23) \
  D(CheckWeakMapMarking, 24)             \
  D(YieldWhileGrayMarking, 25)

enum class ZealMode {
#define ZEAL_MODE(name, value) name = value,
  JS_FOR_EACH_ZEAL_MODE(ZEAL_MODE)
#undef ZEAL_MODE
      Count,
  Limit = Count - 1
};

} /* namespace gc */

// Reasons we reset an ongoing incremental GC or perform a non-incremental GC.
#define GC_ABORT_REASONS(D)      \
  D(None, 0)                     \
  D(NonIncrementalRequested, 1)  \
  D(AbortRequested, 2)           \
  D(Unused1, 3)                  \
  D(IncrementalDisabled, 4)      \
  D(ModeChange, 5)               \
  D(MallocBytesTrigger, 6)       \
  D(GCBytesTrigger, 7)           \
  D(ZoneChange, 8)               \
  D(CompartmentRevived, 9)       \
  D(GrayRootBufferingFailed, 10) \
  D(JitCodeBytesTrigger, 11)
enum class GCAbortReason {
#define MAKE_REASON(name, num) name = num,
  GC_ABORT_REASONS(MAKE_REASON)
#undef MAKE_REASON
};

#define JS_FOR_EACH_INTERNAL_MEMORY_USE(_) \
  _(ArrayBufferContents)                   \
  _(StringContents)                        \
  _(ObjectElements)                        \
  _(ObjectSlots)                           \
  _(ScriptPrivateData)                     \
  _(MapObjectTable)                        \
  _(BigIntDigits)                          \
  _(ScopeData)                             \
  _(WeakMapObject)                         \
  _(ShapeSetForAdd)                        \
  _(PropMapChildren)                       \
  _(PropMapTable)                          \
  _(ModuleBindingMap)                      \
  _(ModuleCyclicFields)                    \
  _(ModuleSyntheticFields)                 \
  _(ModuleExports)                         \
  _(ModuleImportAttributes)                \
  _(BaselineScript)                        \
  _(IonScript)                             \
  _(ArgumentsData)                         \
  _(RareArgumentsData)                     \
  _(RegExpSharedBytecode)                  \
  _(RegExpSharedNamedCaptureData)          \
  _(RegExpSharedNamedCaptureSliceData)     \
  _(TypedArrayElements)                    \
  _(NativeIterator)                        \
  _(JitScript)                             \
  _(ScriptDebugScript)                     \
  _(BreakpointSite)                        \
  _(Breakpoint)                            \
  _(ForOfPIC)                              \
  _(ForOfPICStub)                          \
  _(WasmInstanceExports)                   \
  _(WasmInstanceScopes)                    \
  _(WasmInstanceGlobals)                   \
  _(WasmInstanceInstance)                  \
  _(WasmMemoryObservers)                   \
  _(WasmGlobalCell)                        \
  _(WasmResolveResponseClosure)            \
  _(WasmModule)                            \
  _(WasmTableTable)                        \
  _(WasmExceptionData)                     \
  _(WasmTagType)                           \
  _(FileObjectFile)                        \
  _(Debugger)                              \
  _(DebuggerFrameGeneratorInfo)            \
  _(DebuggerFrameIterData)                 \
  _(DebuggerOnStepHandler)                 \
  _(DebuggerOnPopHandler)                  \
  _(ICUObject)                             \
  _(FinalizationRegistryRecordVector)      \
  _(FinalizationRegistryRegistrations)     \
  _(FinalizationRecordVector)              \
  _(TrackedAllocPolicy)                    \
  _(SharedArrayRawBuffer)                  \
  _(XDRBufferElements)                     \
  _(GlobalObjectData)                      \
  _(ProxyExternalValueArray)               \
  _(WasmTrailerBlock)

#define JS_FOR_EACH_MEMORY_USE(_)  \
  JS_FOR_EACH_PUBLIC_MEMORY_USE(_) \
  JS_FOR_EACH_INTERNAL_MEMORY_USE(_)

enum class MemoryUse : uint8_t {
#define DEFINE_MEMORY_USE(Name) Name,
  JS_FOR_EACH_MEMORY_USE(DEFINE_MEMORY_USE)
#undef DEFINE_MEMORY_USE
};

} /* namespace js */

#endif /* gc_GCEnum_h */

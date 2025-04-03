/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CompileWrappers_h
#define jit_CompileWrappers_h

#include <stdint.h>

#include "gc/Pretenuring.h"
#include "js/TypeDecls.h"

struct JSAtomState;

namespace mozilla::non_crypto {
class XorShift128PlusRNG;
}

namespace JS {
enum class TraceKind;
}

namespace js {

class GeckoProfilerRuntime;
class GlobalObject;
struct JSDOMCallbacks;
class PropertyName;
class StaticStrings;
struct WellKnownSymbols;

using DOMCallbacks = struct JSDOMCallbacks;

namespace gc {

enum class AllocKind : uint8_t;

class FreeSpan;

}  // namespace gc

namespace jit {

class JitRuntime;

// During Ion compilation we need access to various bits of the current
// compartment, runtime and so forth. However, since compilation can run off
// thread while the main thread is mutating the VM, this access needs
// to be restricted. The classes below give the compiler an interface to access
// all necessary information in a threadsafe fashion.

class CompileRuntime {
  JSRuntime* runtime();

 public:
  static CompileRuntime* get(JSRuntime* rt);

#ifdef JS_GC_ZEAL
  const uint32_t* addressOfGCZealModeBits();
#endif

  const JitRuntime* jitRuntime();

  // Compilation does not occur off thread when the Gecko Profiler is enabled.
  GeckoProfilerRuntime& geckoProfiler();

  bool hadOutOfMemory();
  bool profilingScripts();

  const JSAtomState& names();
  const PropertyName* emptyString();
  const StaticStrings& staticStrings();
  const WellKnownSymbols& wellKnownSymbols();
  const JSClass* maybeWindowProxyClass();

  const void* mainContextPtr();
  const void* addressOfJitStackLimit();
  const void* addressOfInterruptBits();
  const void* addressOfZone();
  const void* addressOfMegamorphicCache();
  const void* addressOfMegamorphicSetPropCache();
  const void* addressOfStringToAtomCache();
  const void* addressOfLastBufferedWholeCell();

#ifdef DEBUG
  const void* addressOfIonBailAfterCounter();
#endif

  // DOM callbacks must be threadsafe (and will hopefully be removed soon).
  const DOMCallbacks* DOMcallbacks();

  bool runtimeMatches(JSRuntime* rt);
};

class CompileZone {
  friend class MacroAssembler;
  JS::Zone* zone();

 public:
  static CompileZone* get(JS::Zone* zone);

  CompileRuntime* runtime();
  bool isAtomsZone();

  const uint32_t* addressOfNeedsIncrementalBarrier();
  uint32_t* addressOfTenuredAllocCount();
  gc::FreeSpan** addressOfFreeList(gc::AllocKind allocKind);
  bool allocNurseryObjects();
  bool allocNurseryStrings();
  bool allocNurseryBigInts();
  void* addressOfNurseryPosition();

  void* addressOfNurseryAllocatedSites();

  bool canNurseryAllocateStrings();
  bool canNurseryAllocateBigInts();

  gc::AllocSite* catchAllAllocSite(JS::TraceKind traceKind,
                                   gc::CatchAllAllocSite siteKind);
};

class JitRealm;

class CompileRealm {
  JS::Realm* realm();

 public:
  static CompileRealm* get(JS::Realm* realm);

  CompileZone* zone();
  CompileRuntime* runtime();

  const void* realmPtr() { return realm(); }

  const mozilla::non_crypto::XorShift128PlusRNG*
  addressOfRandomNumberGenerator();

  const JitRealm* jitRealm();

  const GlobalObject* maybeGlobal();
  const uint32_t* addressOfGlobalWriteBarriered();

  bool hasAllocationMetadataBuilder();
};

class JitCompileOptions {
 public:
  JitCompileOptions();
  explicit JitCompileOptions(JSContext* cx);

  bool profilerSlowAssertionsEnabled() const {
    return profilerSlowAssertionsEnabled_;
  }

  bool offThreadCompilationAvailable() const {
    return offThreadCompilationAvailable_;
  }

#ifdef DEBUG
  bool ionBailAfterEnabled() const { return ionBailAfterEnabled_; }
#endif

 private:
  bool profilerSlowAssertionsEnabled_;
  bool offThreadCompilationAvailable_;
#ifdef DEBUG
  bool ionBailAfterEnabled_ = false;
#endif
};

}  // namespace jit
}  // namespace js

#endif  // jit_CompileWrappers_h

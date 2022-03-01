/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/CompileWrappers.h"

#include "gc/GC.h"
#include "gc/Heap.h"
#include "jit/Ion.h"
#include "jit/JitRuntime.h"

#include "vm/Realm-inl.h"

using namespace js;
using namespace js::jit;

JSRuntime* CompileRuntime::runtime() {
  return reinterpret_cast<JSRuntime*>(this);
}

/* static */
CompileRuntime* CompileRuntime::get(JSRuntime* rt) {
  return reinterpret_cast<CompileRuntime*>(rt);
}

#ifdef JS_GC_ZEAL
const uint32_t* CompileRuntime::addressOfGCZealModeBits() {
  return runtime()->gc.addressOfZealModeBits();
}
#endif

const JitRuntime* CompileRuntime::jitRuntime() {
  return runtime()->jitRuntime();
}

GeckoProfilerRuntime& CompileRuntime::geckoProfiler() {
  return runtime()->geckoProfiler();
}

bool CompileRuntime::hadOutOfMemory() { return runtime()->hadOutOfMemory; }

bool CompileRuntime::profilingScripts() { return runtime()->profilingScripts; }

const JSAtomState& CompileRuntime::names() { return *runtime()->commonNames; }

const PropertyName* CompileRuntime::emptyString() {
  return runtime()->emptyString;
}

const StaticStrings& CompileRuntime::staticStrings() {
  return *runtime()->staticStrings;
}

const WellKnownSymbols& CompileRuntime::wellKnownSymbols() {
  return *runtime()->wellKnownSymbols;
}

const JSClass* CompileRuntime::maybeWindowProxyClass() {
  return runtime()->maybeWindowProxyClass();
}

const void* CompileRuntime::mainContextPtr() {
  return runtime()->mainContextFromAnyThread();
}

uint32_t* CompileRuntime::addressOfTenuredAllocCount() {
  return runtime()->mainContextFromAnyThread()->addressOfTenuredAllocCount();
}

const void* CompileRuntime::addressOfJitStackLimit() {
  return runtime()->mainContextFromAnyThread()->addressOfJitStackLimit();
}

const void* CompileRuntime::addressOfInterruptBits() {
  return runtime()->mainContextFromAnyThread()->addressOfInterruptBits();
}

const void* CompileRuntime::addressOfZone() {
  return runtime()->mainContextFromAnyThread()->addressOfZone();
}

const DOMCallbacks* CompileRuntime::DOMcallbacks() {
  return runtime()->DOMcallbacks;
}

bool CompileRuntime::runtimeMatches(JSRuntime* rt) { return rt == runtime(); }

Zone* CompileZone::zone() { return reinterpret_cast<Zone*>(this); }

/* static */
CompileZone* CompileZone::get(Zone* zone) {
  return reinterpret_cast<CompileZone*>(zone);
}

CompileRuntime* CompileZone::runtime() {
  return CompileRuntime::get(zone()->runtimeFromAnyThread());
}

bool CompileZone::isAtomsZone() { return zone()->isAtomsZone(); }

#ifdef DEBUG
const void* CompileRuntime::addressOfIonBailAfterCounter() {
  return runtime()->jitRuntime()->addressOfIonBailAfterCounter();
}
#endif

const uint32_t* CompileZone::addressOfNeedsIncrementalBarrier() {
  return zone()->addressOfNeedsIncrementalBarrier();
}

gc::FreeSpan** CompileZone::addressOfFreeList(gc::AllocKind allocKind) {
  return zone()->arenas.addressOfFreeList(allocKind);
}

void* CompileZone::addressOfNurseryPosition() {
  return zone()->runtimeFromAnyThread()->gc.addressOfNurseryPosition();
}

void* CompileZone::addressOfStringNurseryPosition() {
  // Objects and strings share a nursery, for now at least.
  return zone()->runtimeFromAnyThread()->gc.addressOfNurseryPosition();
}

void* CompileZone::addressOfBigIntNurseryPosition() {
  // Objects and BigInts share a nursery, for now at least.
  return zone()->runtimeFromAnyThread()->gc.addressOfNurseryPosition();
}

const void* CompileZone::addressOfNurseryCurrentEnd() {
  return zone()->runtimeFromAnyThread()->gc.addressOfNurseryCurrentEnd();
}

const void* CompileZone::addressOfStringNurseryCurrentEnd() {
  // Although objects and strings share a nursery (and this may change)
  // there is still a separate string end address.  The only time it
  // is different from the regular end address, is when nursery strings are
  // disabled (it will be NULL).
  //
  // This function returns _a pointer to_ that end address.
  return zone()->runtimeFromAnyThread()->gc.addressOfStringNurseryCurrentEnd();
}

const void* CompileZone::addressOfBigIntNurseryCurrentEnd() {
  // Similar to Strings, BigInts also share the nursery with other nursery
  // allocatable things.
  return zone()->runtimeFromAnyThread()->gc.addressOfBigIntNurseryCurrentEnd();
}

uint32_t* CompileZone::addressOfNurseryAllocCount() {
  return zone()->runtimeFromAnyThread()->gc.addressOfNurseryAllocCount();
}

void* CompileZone::addressOfNurseryAllocatedSites() {
  JSRuntime* rt = zone()->runtimeFromAnyThread();
  return rt->gc.nursery().addressOfNurseryAllocatedSites();
}

bool CompileZone::canNurseryAllocateStrings() {
  return zone()->runtimeFromAnyThread()->gc.nursery().canAllocateStrings() &&
         zone()->allocNurseryStrings;
}

bool CompileZone::canNurseryAllocateBigInts() {
  return zone()->runtimeFromAnyThread()->gc.nursery().canAllocateBigInts() &&
         zone()->allocNurseryBigInts;
}

uintptr_t CompileZone::nurseryCellHeader(JS::TraceKind traceKind,
                                         gc::CatchAllAllocSite siteKind) {
  gc::AllocSite* site = siteKind == gc::CatchAllAllocSite::Optimized
                            ? zone()->optimizedAllocSite()
                            : zone()->unknownAllocSite();
  return gc::NurseryCellHeader::MakeValue(site, traceKind);
}

JS::Realm* CompileRealm::realm() { return reinterpret_cast<JS::Realm*>(this); }

/* static */
CompileRealm* CompileRealm::get(JS::Realm* realm) {
  return reinterpret_cast<CompileRealm*>(realm);
}

CompileZone* CompileRealm::zone() { return CompileZone::get(realm()->zone()); }

CompileRuntime* CompileRealm::runtime() {
  return CompileRuntime::get(realm()->runtimeFromAnyThread());
}

const mozilla::non_crypto::XorShift128PlusRNG*
CompileRealm::addressOfRandomNumberGenerator() {
  return realm()->addressOfRandomNumberGenerator();
}

const JitRealm* CompileRealm::jitRealm() { return realm()->jitRealm(); }

const GlobalObject* CompileRealm::maybeGlobal() {
  // This uses unsafeUnbarrieredMaybeGlobal() so as not to trigger the read
  // barrier on the global from off thread.  This is safe because we
  // abort Ion compilation when we GC.
  return realm()->unsafeUnbarrieredMaybeGlobal();
}

const uint32_t* CompileRealm::addressOfGlobalWriteBarriered() {
  return &realm()->globalWriteBarriered;
}

bool CompileRealm::hasAllocationMetadataBuilder() {
  return realm()->hasAllocationMetadataBuilder();
}

JitCompileOptions::JitCompileOptions()
    : profilerSlowAssertionsEnabled_(false),
      offThreadCompilationAvailable_(false) {}

JitCompileOptions::JitCompileOptions(JSContext* cx) {
  profilerSlowAssertionsEnabled_ =
      cx->runtime()->geckoProfiler().enabled() &&
      cx->runtime()->geckoProfiler().slowAssertionsEnabled();
  offThreadCompilationAvailable_ = OffThreadCompilationAvailable(cx);
#ifdef DEBUG
  ionBailAfterEnabled_ = cx->runtime()->jitRuntime()->ionBailAfterEnabled();
#endif
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Ion.h"

using namespace js;
using namespace js::jit;

JSRuntime*
CompileRuntime::runtime()
{
    return reinterpret_cast<JSRuntime*>(this);
}

/* static */ CompileRuntime*
CompileRuntime::get(JSRuntime* rt)
{
    return reinterpret_cast<CompileRuntime*>(rt);
}

bool
CompileRuntime::onMainThread()
{
    return js::CurrentThreadCanAccessRuntime(runtime());
}

js::PerThreadData*
CompileRuntime::mainThread()
{
    MOZ_ASSERT(onMainThread());
    return &runtime()->mainThread;
}

const void*
CompileRuntime::addressOfJitTop()
{
    return &runtime()->jitTop;
}

const void*
CompileRuntime::addressOfJitActivation()
{
    return &runtime()->jitActivation;
}

const void*
CompileRuntime::addressOfProfilingActivation()
{
    return (const void*) &runtime()->profilingActivation_;
}

const void*
CompileRuntime::addressOfJitStackLimit()
{
    return runtime()->addressOfJitStackLimit();
}

const void*
CompileRuntime::addressOfJSContext()
{
    return &runtime()->jitJSContext;
}

const void*
CompileRuntime::addressOfActivation()
{
    return runtime()->addressOfActivation();
}

const void*
CompileRuntime::addressOfLastCachedNativeIterator()
{
    return &runtime()->nativeIterCache.last;
}

#ifdef JS_GC_ZEAL
const void*
CompileRuntime::addressOfGCZeal()
{
    return runtime()->gc.addressOfZealMode();
}
#endif

const void*
CompileRuntime::addressOfInterruptUint32()
{
    return runtime()->addressOfInterruptUint32();
}

const JitRuntime*
CompileRuntime::jitRuntime()
{
    return runtime()->jitRuntime();
}

SPSProfiler&
CompileRuntime::spsProfiler()
{
    return runtime()->spsProfiler;
}

bool
CompileRuntime::canUseSignalHandlers()
{
    return runtime()->canUseSignalHandlers();
}

bool
CompileRuntime::jitSupportsFloatingPoint()
{
    return runtime()->jitSupportsFloatingPoint;
}

bool
CompileRuntime::hadOutOfMemory()
{
    return runtime()->hadOutOfMemory;
}

bool
CompileRuntime::profilingScripts()
{
    return runtime()->profilingScripts;
}

const JSAtomState&
CompileRuntime::names()
{
    return *runtime()->commonNames;
}

const PropertyName*
CompileRuntime::emptyString()
{
    return runtime()->emptyString;
}

const StaticStrings&
CompileRuntime::staticStrings()
{
    return *runtime()->staticStrings;
}

const Value&
CompileRuntime::NaNValue()
{
    return runtime()->NaNValue;
}

const Value&
CompileRuntime::positiveInfinityValue()
{
    return runtime()->positiveInfinityValue;
}

const WellKnownSymbols&
CompileRuntime::wellKnownSymbols()
{
    MOZ_ASSERT(onMainThread());
    return *runtime()->wellKnownSymbols;
}

#ifdef DEBUG
bool
CompileRuntime::isInsideNursery(gc::Cell* cell)
{
    return UninlinedIsInsideNursery(cell);
}
#endif

const DOMCallbacks*
CompileRuntime::DOMcallbacks()
{
    return GetDOMCallbacks(runtime());
}

const MathCache*
CompileRuntime::maybeGetMathCache()
{
    return runtime()->maybeGetMathCache();
}

const Nursery&
CompileRuntime::gcNursery()
{
    return runtime()->gc.nursery;
}

void
CompileRuntime::setMinorGCShouldCancelIonCompilations()
{
    MOZ_ASSERT(onMainThread());
    runtime()->gc.storeBuffer.setShouldCancelIonCompilations();
}

Zone*
CompileZone::zone()
{
    return reinterpret_cast<Zone*>(this);
}

/* static */ CompileZone*
CompileZone::get(Zone* zone)
{
    return reinterpret_cast<CompileZone*>(zone);
}

const void*
CompileZone::addressOfNeedsIncrementalBarrier()
{
    return zone()->addressOfNeedsIncrementalBarrier();
}

const void*
CompileZone::addressOfFreeListFirst(gc::AllocKind allocKind)
{
    return zone()->arenas.getFreeList(allocKind)->addressOfFirst();
}

const void*
CompileZone::addressOfFreeListLast(gc::AllocKind allocKind)
{
    return zone()->arenas.getFreeList(allocKind)->addressOfLast();
}

JSCompartment*
CompileCompartment::compartment()
{
    return reinterpret_cast<JSCompartment*>(this);
}

/* static */ CompileCompartment*
CompileCompartment::get(JSCompartment* comp)
{
    return reinterpret_cast<CompileCompartment*>(comp);
}

CompileZone*
CompileCompartment::zone()
{
    return CompileZone::get(compartment()->zone());
}

CompileRuntime*
CompileCompartment::runtime()
{
    return CompileRuntime::get(compartment()->runtimeFromAnyThread());
}

const void*
CompileCompartment::addressOfEnumerators()
{
    return &compartment()->enumerators;
}

const void*
CompileCompartment::addressOfRandomNumberGenerator()
{
    return compartment()->randomNumberGenerator.ptr();
}

const JitCompartment*
CompileCompartment::jitCompartment()
{
    return compartment()->jitCompartment();
}

bool
CompileCompartment::hasObjectMetadataCallback()
{
    return compartment()->hasObjectMetadataCallback();
}

// Note: This function is thread-safe because setSingletonAsValue sets a boolean
// variable to false, and this boolean variable has no way to be resetted to
// true. So even if there is a concurrent write, this concurrent write will
// always have the same value.  If there is a concurrent read, then we will
// clone a singleton instead of using the value which is baked in the JSScript,
// and this would be an unfortunate allocation, but this will not change the
// semantics of the JavaScript code which is executed.
void
CompileCompartment::setSingletonsAsValues()
{
    return JS::CompartmentOptionsRef(compartment()).setSingletonsAsValues();
}

JitCompileOptions::JitCompileOptions()
  : cloneSingletons_(false),
    spsSlowAssertionsEnabled_(false),
    offThreadCompilationAvailable_(false)
{
}

JitCompileOptions::JitCompileOptions(JSContext* cx)
{
    JS::CompartmentOptions& options = cx->compartment()->options();
    cloneSingletons_ = options.cloneSingletons();
    spsSlowAssertionsEnabled_ = cx->runtime()->spsProfiler.enabled() &&
                                cx->runtime()->spsProfiler.slowAssertionsEnabled();
    offThreadCompilationAvailable_ = OffThreadCompilationAvailable(cx);
}

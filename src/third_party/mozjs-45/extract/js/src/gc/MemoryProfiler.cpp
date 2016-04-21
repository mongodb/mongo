/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsfriendapi.h"

#include "vm/Runtime.h"

using js::gc::Cell;

mozilla::Atomic<uint32_t, mozilla::Relaxed> MemProfiler::sActiveProfilerCount;
NativeProfiler* MemProfiler::sNativeProfiler;

GCHeapProfiler*
MemProfiler::GetGCHeapProfiler(void* addr)
{
    JSRuntime* runtime = reinterpret_cast<Cell*>(addr)->runtimeFromAnyThread();
    return runtime->gc.mMemProfiler.mGCHeapProfiler;
}

GCHeapProfiler*
MemProfiler::GetGCHeapProfiler(JSRuntime* runtime)
{
    return runtime->gc.mMemProfiler.mGCHeapProfiler;
}

MemProfiler*
MemProfiler::GetMemProfiler(JSRuntime* runtime)
{
    return &runtime->gc.mMemProfiler;
}

void
MemProfiler::start(GCHeapProfiler* aGCHeapProfiler)
{
    ReleaseAllJITCode(mRuntime->defaultFreeOp());
    mGCHeapProfiler = aGCHeapProfiler;
    sActiveProfilerCount++;
}

void
MemProfiler::stop()
{
    sActiveProfilerCount--;
    mGCHeapProfiler = nullptr;
}

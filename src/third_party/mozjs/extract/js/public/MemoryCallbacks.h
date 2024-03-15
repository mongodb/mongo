/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_MemoryCallbacks_h
#define js_MemoryCallbacks_h

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace JS {

/**
 * If a large allocation fails when calling pod_{calloc,realloc}CanGC, the JS
 * engine may call the large-allocation-failure callback, if set, to allow the
 * embedding to flush caches, possibly perform shrinking GCs, etc. to make some
 * room. The allocation will then be retried (and may still fail.) This callback
 * can be called on any thread and must be set at most once in a process.
 */

using LargeAllocationFailureCallback = void (*)();

extern JS_PUBLIC_API void SetProcessLargeAllocationFailureCallback(
    LargeAllocationFailureCallback afc);

/**
 * Unlike the error reporter, which is only called if the exception for an OOM
 * bubbles up and is not caught, the OutOfMemoryCallback is called immediately
 * at the OOM site to allow the embedding to capture the current state of heap
 * allocation before anything is freed. If the large-allocation-failure callback
 * is called at all (not all allocation sites call the large-allocation-failure
 * callback on failure), it is called before the out-of-memory callback; the
 * out-of-memory callback is only called if the allocation still fails after the
 * large-allocation-failure callback has returned.
 */

using OutOfMemoryCallback = void (*)(JSContext*, void*);

extern JS_PUBLIC_API void SetOutOfMemoryCallback(JSContext* cx,
                                                 OutOfMemoryCallback cb,
                                                 void* data);

}  // namespace JS

#endif  // js_MemoryCallbacks_h

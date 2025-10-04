/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Embedder-supplied tracking of the allocation and deallocation of various
 * non-garbage-collected objects in SpiderMonkey.
 *
 * This functionality is intended to track allocation of non-user-visible
 * structures, for debugging the C++ of an embedding.  It is not intended to
 * give the end user visibility into allocations in JS.  Instead see
 * AllocationRecording.h for such functionality.
 */

#ifndef js_AllocationLogging_h
#define js_AllocationLogging_h

#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

namespace JS {

using LogCtorDtor = void (*)(void*, const char*, uint32_t);

/**
 * Set global functions used to monitor classes to highlight leaks.
 *
 * For each C++ class that uses these mechanisms, the allocation of an instance
 * will log the constructor call, and its subsequent deallocation will log the
 * destructor call.  If only the former occurs, the instance/allocation is
 * leaked.  With carefully-written logging functions, this can be used to debug
 * the origin of the leaks.
 */
extern JS_PUBLIC_API void SetLogCtorDtorFunctions(LogCtorDtor ctor,
                                                  LogCtorDtor dtor);

/**
 * Log the allocation of |self|, having a type uniquely identified by the string
 * |type|, with allocation size |sz|.
 *
 * You generally should use |JS_COUNT_CTOR| and |JS_COUNT_DTOR| instead of
 * using this function directly.
 */
extern JS_PUBLIC_API void LogCtor(void* self, const char* type, uint32_t sz);

/**
 * Log the deallocation of |self|, having a type uniquely identified by the
 * string |type|, with allocation size |sz|.
 *
 * You generally should use |JS_COUNT_CTOR| and |JS_COUNT_DTOR| instead of
 * using this function directly.
 */
extern JS_PUBLIC_API void LogDtor(void* self, const char* type, uint32_t sz);

/**
 * Within each non-delegating constructor of a |Class|, use
 * |JS_COUNT_CTOR(Class);| to log the allocation of |this|.  (If you do this in
 * delegating constructors, you might count a single allocation multiple times.)
 */
#define JS_COUNT_CTOR(Class) \
  (::JS::LogCtor(static_cast<void*>(this), #Class, sizeof(Class)))

/**
 * Within the destructor of a |Class|, use |JS_COUNT_DTOR(Class);| to log the
 * deallocation of |this|.
 */
#define JS_COUNT_DTOR(Class) \
  (::JS::LogDtor(static_cast<void*>(this), #Class, sizeof(Class)))

}  // namespace JS

#endif  // js_AllocationLogging_h

/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/scripting/mozjs/implscope.h"

#include <jscustomallocator.h>
#include <jscustomallocator_oom.h>
#include <jstypes.h>
#include <mozjemalloc_types.h>

#include <mozilla/Assertions.h>

#ifdef __linux__
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(_WIN32)
#include <malloc.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#else
#error "Unsupported platform"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

/**
 * This shim interface (which controls dynamic allocation within SpiderMonkey),
 * consciously uses std::malloc and friends over mongoMalloc. It does this
 * because SpiderMonkey has some plausible options in the event of OOM,
 * specifically it can begin aggressive garbage collection. It would also be
 * reasonable to go the other route and fail, but for the moment I erred on the
 * side of maintaining the contract that SpiderMonkey expects.
 *
 * The overall strategy here is to keep track of allocations in a thread local,
 * offering us the chance to enforce soft limits on memory use rather than
 * waiting for the OS to OOM us.
 */

namespace mongo {
namespace sm {

namespace {
/**
 * These two variables track the total number of bytes handed out, and the
 * maximum number of bytes we will consider handing out. They are set by
 * MozJSImplScope on start up.
 */
thread_local size_t malloc_bytes = 0;
thread_local size_t max_bytes = 0;

// Allow disabling mmap tracking as a fallback in case counted memory goes too high
thread_local bool track_mmap_bytes = true;
}  // namespace

size_t get_total_bytes() {
    return get_malloc_bytes() + get_mmap_bytes();
}

size_t get_malloc_bytes() {
    return malloc_bytes;
}

size_t get_mmap_bytes() {
    return track_mmap_bytes ? js::gc::GetProfilerMemoryCounts().bytes : 0;
}

void reset(size_t bytes, bool track_mmap) {
    malloc_bytes = 0;
    max_bytes = bytes;
    track_mmap_bytes = track_mmap;
}

size_t get_max_bytes() {
    return max_bytes;
}

void signal_oom() {
    auto scope = mongo::mozjs::MozJSImplScope::getThreadScope();
    if (scope) {
        scope->setOOM();
    }
}

void check_oom_on_mmap_allocation(size_t bytes) {
    // called after an mmap allocation to check whether an oom signal is required
    // if track_mmap_bytes is disabled we fall back to legacy logic and completely bypass this path
    size_t mb = get_max_bytes();
    size_t tb = get_total_bytes();
    if (track_mmap_bytes && mb > 0 && get_total_bytes() + bytes > mb) {
        LOGV2(10920001,
              "Out of memory on MozJS mmap allocation",
              "bytes"_attr = bytes,
              "totalBytes"_attr = tb,
              "maxBytes"_attr = mb);
        signal_oom();
    }
}

size_t get_current(void* ptr);

/**
 * Wraps std::Xalloc functions
 *
 * The idea here is to abstract soft limits on allocations.
 */
template <typename T>
void* wrap_alloc(T&& func, void* ptr, size_t bytes) {
    size_t mb = get_max_bytes();
    size_t mlb = get_malloc_bytes();
    size_t mmb = get_mmap_bytes();

    // During a GC cycle, GC::purgeRuntime() is called, which tries to free unused items in the
    // SharedImmutableStringsCache while holding its corresponding mutex. Our js_free implementation
    // calls wrap_alloc, with a value of 0 for 'bytes'. Previously, if we were already at the
    // max_bytes limit when purging the runtime, the call to MozJSImplScope::setOOM() would request
    // an urgent JS interrupt, which acquires a futex with order 500, while still holding the mutex
    // for the SharedImmutableStringsCache (order 600). This triggered a failure of a MOZ_ASSERT
    // which enforces correct lock ordering in the JS engine. For this reason, we avoid checking
    // for an OOM here if we are requesting zero bytes (i.e freeing memory).
    if (mb && bytes && (mlb + mmb + bytes > mb)) {
        bool oomUnsafe = js::AutoEnterOOMUnsafeRegion::isInOOMUnsafeRegion();
        LOGV2(10920000,
              "Out of memory on MozJS malloc allocation",
              "bytes"_attr = bytes,
              "totalBytes"_attr = mlb + mmb,
              "maxBytes"_attr = mb,
              "isInOOMUnsafeRegion"_attr = oomUnsafe);
        signal_oom();

        // In some contexts MozJS has no safe recovery from OOM and crashes the process to avoid
        // unsafe behavior. In that case allow the allocation to proceed and go over the allowed
        // memory limit. Otherwise fail immediately.
        if (!oomUnsafe) {
            return nullptr;
        }
    }

    void* p = func(ptr, bytes);

#if __has_feature(address_sanitizer)
    {
        auto& handles = mongo::mozjs::MozJSImplScope::ASANHandles::getInstance();
        if (bytes) {
            if (ptr) {
                // realloc
                if (ptr != p) {
                    // actually moved the allocation
                    handles.removePointer(ptr);
                    handles.addPointer(p);
                }
                // else we didn't need to realloc, don't have to register
            } else {
                // malloc/calloc
                handles.addPointer(p);
            }
        } else {
            // free
            handles.removePointer(ptr);
        }
    }
#endif

    if (!p) {
        return nullptr;
    }

    malloc_bytes += mongo::sm::get_current(p);
    return p;
}

/*
 * gets the current available size at this pointer, which may be larger than the allocated size.
 * this size is not valid to access unless realloc() is called
 */
size_t get_current(void* ptr) {
#if defined(__linux__) || defined(__FreeBSD__)
    return malloc_usable_size(ptr);
#elif defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize(ptr);
#else
#error "Should be unreachable"
#endif
}

}  // namespace sm
}  // namespace mongo

JS_PUBLIC_DATA arena_id_t js::MallocArena;
JS_PUBLIC_DATA arena_id_t js::BackgroundMallocArena;
JS_PUBLIC_DATA arena_id_t js::ArrayBufferContentsArena;
JS_PUBLIC_DATA arena_id_t js::StringBufferArena;

thread_local size_t js::AutoEnterOOMUnsafeRegion::isInOomUnsafeRegion_ = 0;

void* mongo_arena_malloc(size_t bytes) {
    return std::malloc(bytes);
}

void* mongo_arena_calloc(size_t bytes) {
    return std::calloc(bytes, 1);
}

void* mongo_arena_realloc(void* p, size_t bytes) {
    MOZ_ASSERT(bytes != 0);  // realloc() with zero size is unsupported

    if (!p) {
        return mongo_arena_malloc(bytes);
    }

    // Like in mongo_free, this count is imprecise because get_current calls malloc_usable_size
    // which can return a larger value than the initial allocation
    size_t current = mongo::sm::get_current(p);

    void* ptr = std::realloc(p, bytes);
    if (!ptr) {
        // on failure the old ptr isn't freed, no need to adjust malloc_bytes
        return nullptr;
    }

    mongo::sm::malloc_bytes -= std::min(mongo::sm::malloc_bytes, current);
    return ptr;
}

void* mongo_free(void* ptr) {
    // Note that malloc_usable_size and equivalents can return a larger size than the allocated
    // buffer, so this may result in undercounting
    size_t current = mongo::sm::get_current(ptr);

    mongo::sm::malloc_bytes -= std::min(mongo::sm::malloc_bytes, current);

    std::free(ptr);
    return nullptr;
}

void* js_arena_malloc(size_t arena, size_t bytes) {
    JS_OOM_POSSIBLY_FAIL();
    JS_CHECK_LARGE_ALLOC(bytes);
    return mongo::sm::wrap_alloc(
        [](void* ptr, size_t b) { return mongo_arena_malloc(b); }, nullptr, bytes);
}

void* js_malloc(size_t bytes) {
    return js_arena_malloc(js::MallocArena, bytes);
}

void* js_arena_calloc(arena_id_t arena, size_t bytes) {
    JS_OOM_POSSIBLY_FAIL();
    JS_CHECK_LARGE_ALLOC(bytes);
    return mongo::sm::wrap_alloc(
        [](void* ptr, size_t b) { return mongo_arena_calloc(b); }, nullptr, bytes);
}

void* js_arena_calloc(arena_id_t arena, size_t nmemb, size_t size) {
    JS_OOM_POSSIBLY_FAIL();
    JS_CHECK_LARGE_ALLOC(size);
    return mongo::sm::wrap_alloc(
        [](void* ptr, size_t b) { return mongo_arena_calloc(b); }, nullptr, size * nmemb);
}

void* js_calloc(size_t bytes) {
    return js_arena_calloc(js::MallocArena, bytes);
}

void* js_calloc(size_t nmemb, size_t size) {
    return js_arena_calloc(js::MallocArena, nmemb, size);
}

void* js_arena_realloc(arena_id_t arena, void* p, size_t bytes) {
    // realloc() with zero size is not portable, as some implementations may
    // return nullptr on success and free |p| for this.  We assume nullptr
    // indicates failure and that |p| is still valid.
    MOZ_ASSERT(bytes != 0);

    JS_OOM_POSSIBLY_FAIL();
    JS_CHECK_LARGE_ALLOC(bytes);
    return mongo::sm::wrap_alloc(
        [](void* ptr, size_t b) { return mongo_arena_realloc(ptr, b); }, p, bytes);
}

void* js_realloc(void* p, size_t bytes) {
    return js_arena_realloc(js::MallocArena, p, bytes);
}

void js_free(void* p) {
    if (!p)
        return;

    mongo::sm::wrap_alloc([](void* ptr, size_t b) { return mongo_free(ptr); }, p, 0);
}

void js::InitMallocAllocator() {
    MallocArena = 0;
    BackgroundMallocArena = 1;
    ArrayBufferContentsArena = 2;
    StringBufferArena = 3;
}

void js::ShutDownMallocAllocator() {}

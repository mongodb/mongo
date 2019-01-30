
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

#include "mongo/platform/basic.h"

#include <cstddef>
#include <jscustomallocator.h>
#include <type_traits>

#include "mongo/config.h"
#include "mongo/scripting/mozjs/implscope.h"

#ifdef __linux__
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(_WIN32)
#include <malloc.h>
#else
#define MONGO_NO_MALLOC_USABLE_SIZE
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

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
thread_local size_t total_bytes = 0;
thread_local size_t max_bytes = 0;

/**
 * When we don't have malloc_usable_size, we manage by adjusting our pointer by
 * kMaxAlign bytes and storing the size of the allocation kMaxAlign bytes
 * behind the pointer we hand back. That let's us get to the value at runtime.
 * We know kMaxAlign is enough (generally 8 or 16 bytes), because that's
 * literally the contract between malloc and std::max_align_t.
 *
 * This is commented out right now because std::max_align_t didn't seem to be
 * available on our solaris builder. TODO: revisit in the future to see if that
 * still holds.
 */
// const size_t kMaxAlign = std::alignment_of<std::max_align_t>::value;
const size_t kMaxAlign = 16;
}  // namespace

size_t get_total_bytes() {
    return total_bytes;
}

void reset(size_t bytes) {
    total_bytes = 0;
    max_bytes = bytes;
}

size_t get_max_bytes() {
    return max_bytes;
}

/**
 * Wraps std::Xalloc functions
 *
 * The idea here is to abstract soft limits on allocations, as well as possibly
 * necessary pointer adjustment (if we don't have a malloc_usable_size
 * replacement).
 *
 */
template <typename T>
void* wrap_alloc(T&& func, void* ptr, size_t bytes) {
    size_t mb = get_max_bytes();
    size_t tb = get_total_bytes();

    if (mb && (tb + bytes > mb)) {
        auto scope = mongo::mozjs::MozJSImplScope::getThreadScope();
        if (scope)
            scope->setOOM();

        // We fall through here because we want to let spidermonkey continue
        // with whatever it was doing.  Calling setOOM will fail the top level
        // operation as soon as possible.
    }

#ifdef MONGO_NO_MALLOC_USABLE_SIZE
    ptr = ptr ? static_cast<char*>(ptr) - kMaxAlign : nullptr;
#endif

#ifdef MONGO_NO_MALLOC_USABLE_SIZE
    void* p = func(ptr, bytes + kMaxAlign);
#else
    void* p = func(ptr, bytes);
#endif

#if __has_feature(address_sanitizer)
    {
        auto handles = mongo::mozjs::MozJSImplScope::ASANHandles::getThreadASANHandles();

        if (handles) {
            if (bytes) {
                if (ptr) {
                    // realloc
                    if (ptr != p) {
                        // actually moved the allocation
                        handles->removePointer(ptr);
                        handles->addPointer(p);
                    }
                    // else we didn't need to realloc, don't have to register
                } else {
                    // malloc/calloc
                    handles->addPointer(p);
                }
            } else {
                // free
                handles->removePointer(ptr);
            }
        }
    }
#endif

    if (!p) {
        return nullptr;
    }

#ifdef MONGO_NO_MALLOC_USABLE_SIZE
    *reinterpret_cast<size_t*>(p) = bytes;
    p = static_cast<char*>(p) + kMaxAlign;
#endif

    total_bytes = tb + bytes;

    return p;
}

size_t get_current(void* ptr) {
#ifdef MONGO_NO_MALLOC_USABLE_SIZE
    if (!ptr)
        return 0;

    return *reinterpret_cast<size_t*>(static_cast<char*>(ptr) - kMaxAlign);
#elif defined(__linux__)
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

void* js_malloc(size_t bytes) {
    return mongo::sm::wrap_alloc(
        [](void* ptr, size_t b) { return std::malloc(b); }, nullptr, bytes);
}

void* js_calloc(size_t bytes) {
    return mongo::sm::wrap_alloc(
        [](void* ptr, size_t b) { return std::calloc(b, 1); }, nullptr, bytes);
}

void* js_calloc(size_t nmemb, size_t size) {
    return mongo::sm::wrap_alloc(
        [](void* ptr, size_t b) { return std::calloc(b, 1); }, nullptr, nmemb * size);
}

void js_free(void* p) {
    if (!p)
        return;

    size_t current = mongo::sm::get_current(p);
    size_t tb = mongo::sm::get_total_bytes();

    if (tb >= current) {
        mongo::sm::total_bytes = tb - current;
    }

    mongo::sm::wrap_alloc(
        [](void* ptr, size_t b) {
            std::free(ptr);
            return nullptr;
        },
        p,
        0);
}

void* js_realloc(void* p, size_t bytes) {
    if (!p) {
        return js_malloc(bytes);
    }

    if (!bytes) {
        js_free(p);
        return nullptr;
    }

    size_t current = mongo::sm::get_current(p);

    if (current >= bytes) {
        return p;
    }

    size_t tb = mongo::sm::total_bytes;

    if (tb >= current) {
        mongo::sm::total_bytes = tb - current;
    }

    return mongo::sm::wrap_alloc(
        [](void* ptr, size_t b) { return std::realloc(ptr, b); }, p, bytes);
}

void js::InitMallocAllocator() {}

void js::ShutDownMallocAllocator() {}

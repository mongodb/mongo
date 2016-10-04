/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS allocation policies.
 *
 * The allocators here are for system memory with lifetimes which are not
 * managed by the GC. See the comment at the top of vm/MallocProvider.h.
 */

#ifndef jsalloc_h
#define jsalloc_h

#include "js/TypeDecls.h"
#include "js/Utility.h"

namespace js {

enum class AllocFunction {
    Malloc,
    Calloc,
    Realloc
};

struct ContextFriendFields;

/* Policy for using system memory functions and doing no error reporting. */
class SystemAllocPolicy
{
  public:
    template <typename T> T* maybe_pod_malloc(size_t numElems) { return js_pod_malloc<T>(numElems); }
    template <typename T> T* maybe_pod_calloc(size_t numElems) { return js_pod_calloc<T>(numElems); }
    template <typename T> T* maybe_pod_realloc(T* p, size_t oldSize, size_t newSize) {
        return js_pod_realloc<T>(p, oldSize, newSize);
    }
    template <typename T> T* pod_malloc(size_t numElems) { return maybe_pod_malloc<T>(numElems); }
    template <typename T> T* pod_calloc(size_t numElems) { return maybe_pod_calloc<T>(numElems); }
    template <typename T> T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
        return maybe_pod_realloc<T>(p, oldSize, newSize);
    }
    void free_(void* p) { js_free(p); }
    void reportAllocOverflow() const {}
    bool checkSimulatedOOM() const {
        return !js::oom::ShouldFailWithOOM();
    }
};

class ExclusiveContext;
void ReportOutOfMemory(ExclusiveContext* cxArg);

/*
 * Allocation policy that calls the system memory functions and reports errors
 * to the context. Since the JSContext given on construction is stored for
 * the lifetime of the container, this policy may only be used for containers
 * whose lifetime is a shorter than the given JSContext.
 *
 * FIXME bug 647103 - rewrite this in terms of temporary allocation functions,
 * not the system ones.
 */
class TempAllocPolicy
{
    ContextFriendFields* const cx_;

    /*
     * Non-inline helper to call JSRuntime::onOutOfMemory with minimal
     * code bloat.
     */
    JS_FRIEND_API(void*) onOutOfMemory(AllocFunction allocFunc, size_t nbytes,
                                       void* reallocPtr = nullptr);

    template <typename T>
    T* onOutOfMemoryTyped(AllocFunction allocFunc, size_t numElems, void* reallocPtr = nullptr) {
        size_t bytes;
        if (MOZ_UNLIKELY(!CalculateAllocSize<T>(numElems, &bytes)))
            return nullptr;
        return static_cast<T*>(onOutOfMemory(allocFunc, bytes, reallocPtr));
    }

  public:
    MOZ_IMPLICIT TempAllocPolicy(JSContext* cx) : cx_((ContextFriendFields*) cx) {} // :(
    MOZ_IMPLICIT TempAllocPolicy(ContextFriendFields* cx) : cx_(cx) {}

    template <typename T>
    T* maybe_pod_malloc(size_t numElems) {
        return js_pod_malloc<T>(numElems);
    }

    template <typename T>
    T* maybe_pod_calloc(size_t numElems) {
        return js_pod_calloc<T>(numElems);
    }

    template <typename T>
    T* maybe_pod_realloc(T* prior, size_t oldSize, size_t newSize) {
        return js_pod_realloc<T>(prior, oldSize, newSize);
    }

    template <typename T>
    T* pod_malloc(size_t numElems) {
        T* p = maybe_pod_malloc<T>(numElems);
        if (MOZ_UNLIKELY(!p))
            p = onOutOfMemoryTyped<T>(AllocFunction::Malloc, numElems);
        return p;
    }

    template <typename T>
    T* pod_calloc(size_t numElems) {
        T* p = maybe_pod_calloc<T>(numElems);
        if (MOZ_UNLIKELY(!p))
            p = onOutOfMemoryTyped<T>(AllocFunction::Calloc, numElems);
        return p;
    }

    template <typename T>
    T* pod_realloc(T* prior, size_t oldSize, size_t newSize) {
        T* p2 = maybe_pod_realloc<T>(prior, oldSize, newSize);
        if (MOZ_UNLIKELY(!p2))
            p2 = onOutOfMemoryTyped<T>(AllocFunction::Realloc, newSize, prior);
        return p2;
    }

    void free_(void* p) {
        js_free(p);
    }

    JS_FRIEND_API(void) reportAllocOverflow() const;

    bool checkSimulatedOOM() const {
        if (js::oom::ShouldFailWithOOM()) {
            js::ReportOutOfMemory(reinterpret_cast<ExclusiveContext*>(cx_));
            return false;
        }

        return true;
    }
};

} /* namespace js */

#endif /* jsalloc_h */

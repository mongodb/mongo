/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_SweepingAPI_h
#define js_SweepingAPI_h

#include "js/HeapAPI.h"

namespace JS {
namespace detail {
class WeakCacheBase;
} // namespace detail

namespace shadow {
JS_PUBLIC_API(void)
RegisterWeakCache(JS::Zone* zone, JS::detail::WeakCacheBase* cachep);
JS_PUBLIC_API(void)
RegisterWeakCache(JSRuntime* rt, JS::detail::WeakCacheBase* cachep);
} // namespace shadow

namespace detail {
class WeakCacheBase : public mozilla::LinkedListElement<WeakCacheBase>
{
    WeakCacheBase() = delete;
    explicit WeakCacheBase(const WeakCacheBase&) = delete;

  public:
    explicit WeakCacheBase(Zone* zone) {
        shadow::RegisterWeakCache(zone, this);
    }
    explicit WeakCacheBase(JSRuntime* rt) {
        shadow::RegisterWeakCache(rt, this);
    }
    WeakCacheBase(WeakCacheBase&& other) = default;
    virtual ~WeakCacheBase() {}

    virtual size_t sweep() = 0;
    virtual bool needsSweep() = 0;

    virtual bool setNeedsIncrementalBarrier(bool needs) {
        // Derived classes do not support incremental barriers by default.
        return false;
    }
    virtual bool needsIncrementalBarrier() const {
        // Derived classes do not support incremental barriers by default.
        return false;
    }
};
} // namespace detail

// A WeakCache stores the given Sweepable container and links itself into a
// list of such caches that are swept during each GC. A WeakCache can be
// specific to a zone, or across a whole runtime, depending on which
// constructor is used.
template <typename T>
class WeakCache : protected detail::WeakCacheBase,
                  public js::MutableWrappedPtrOperations<T, WeakCache<T>>
{
    T cache;

  public:
    using Type = T;

    template <typename... Args>
    explicit WeakCache(Zone* zone, Args&&... args)
      : WeakCacheBase(zone), cache(mozilla::Forward<Args>(args)...)
    {}
    template <typename... Args>
    explicit WeakCache(JSRuntime* rt, Args&&... args)
      : WeakCacheBase(rt), cache(mozilla::Forward<Args>(args)...)
    {}

    const T& get() const { return cache; }
    T& get() { return cache; }

    size_t sweep() override {
        GCPolicy<T>::sweep(&cache);
        return 0;
    }

    bool needsSweep() override {
        return cache.needsSweep();
    }
};

} // namespace JS

#endif // js_SweepingAPI_h

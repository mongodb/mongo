/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_FreeOp_h
#define gc_FreeOp_h

#include "mozilla/Assertions.h" // MOZ_ASSERT

#include "jsapi.h" // JSFreeOp

#include "jit/ExecutableAllocator.h" // jit::JitPoisonRangeVector
#include "js/AllocPolicy.h" // SystemAllocPolicy
#include "js/Utility.h" // AutoEnterOOMUnsafeRegion, js_free
#include "js/Vector.h" // js::Vector

struct JSRuntime;

namespace js {

/*
 * A FreeOp can do one thing: free memory. For convenience, it has delete_
 * convenience methods that also call destructors.
 *
 * FreeOp is passed to finalizers and other sweep-phase hooks so that we do not
 * need to pass a JSContext to those hooks.
 */
class FreeOp : public JSFreeOp
{
    Vector<void*, 0, SystemAllocPolicy> freeLaterList;
    jit::JitPoisonRangeVector jitPoisonRanges;

  public:
    static FreeOp* get(JSFreeOp* fop) {
        return static_cast<FreeOp*>(fop);
    }

    explicit FreeOp(JSRuntime* maybeRuntime);
    ~FreeOp();

    bool onActiveCooperatingThread() const {
        return runtime_ != nullptr;
    }

    bool maybeOnHelperThread() const {
        // Sometimes background finalization happens on the active thread so
        // runtime_ being null doesn't always mean we are off thread.
        return !runtime_;
    }

    bool isDefaultFreeOp() const;

    void free_(void* p) {
        js_free(p);
    }

    void freeLater(void* p) {
        // FreeOps other than the defaultFreeOp() are constructed on the stack,
        // and won't hold onto the pointers to free indefinitely.
        MOZ_ASSERT(!isDefaultFreeOp());

        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!freeLaterList.append(p))
            oomUnsafe.crash("FreeOp::freeLater");
    }

    bool appendJitPoisonRange(const jit::JitPoisonRange& range) {
        // FreeOps other than the defaultFreeOp() are constructed on the stack,
        // and won't hold onto the pointers to free indefinitely.
        MOZ_ASSERT(!isDefaultFreeOp());

        return jitPoisonRanges.append(range);
    }

    template <class T>
    void delete_(T* p) {
        if (p) {
            p->~T();
            free_(p);
        }
    }
};

} // namespace js

#endif // gc_FreeOp_h

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_DeletePolicy_h
#define gc_DeletePolicy_h

#include "js/TracingAPI.h"

namespace js {
namespace gc {

struct ClearEdgesTracer : public JS::CallbackTracer
{
    ClearEdgesTracer();

#ifdef DEBUG
    TracerKind getTracerKind() const override { return TracerKind::ClearEdges; }
#endif

    template <typename T>
    inline void clearEdge(T** thingp);

    void onObjectEdge(JSObject** objp) override;
    void onStringEdge(JSString** strp) override;
    void onSymbolEdge(JS::Symbol** symp) override;
    void onScriptEdge(JSScript** scriptp) override;
    void onShapeEdge(js::Shape** shapep) override;
    void onObjectGroupEdge(js::ObjectGroup** groupp) override;
    void onBaseShapeEdge(js::BaseShape** basep) override;
    void onJitCodeEdge(js::jit::JitCode** codep) override;
    void onLazyScriptEdge(js::LazyScript** lazyp) override;
    void onScopeEdge(js::Scope** scopep) override;
    void onRegExpSharedEdge(js::RegExpShared** sharedp) override;
    void onChild(const JS::GCCellPtr& thing) override;
};

#ifdef DEBUG
inline bool
IsClearEdgesTracer(JSTracer *trc)
{
    return trc->isCallbackTracer() &&
           trc->asCallbackTracer()->getTracerKind() == JS::CallbackTracer::TracerKind::ClearEdges;
}
#endif

} // namespace gc

/*
 * Provides a delete policy that can be used for objects which have their
 * lifetime managed by the GC so they can be safely destroyed outside of GC.
 *
 * This is necessary for example when initializing such an object may fail after
 * the initial allocation. The partially-initialized object must be destroyed,
 * but it may not be safe to do so at the current time as the store buffer may
 * contain pointers into it.
 *
 * This policy traces GC pointers in the object and clears them, making sure to
 * trigger barriers while doing so. This will remove any store buffer pointers
 * into the object and make it safe to delete.
 */
template <typename T>
struct GCManagedDeletePolicy
{
    void operator()(const T* constPtr) {
        if (constPtr) {
            auto ptr = const_cast<T*>(constPtr);
            gc::ClearEdgesTracer trc;
            ptr->trace(&trc);
            js_delete(ptr);
        }
    }
};

} // namespace js

#endif // gc_DeletePolicy_h

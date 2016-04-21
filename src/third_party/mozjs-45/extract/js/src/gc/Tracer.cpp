/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Tracer.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/SizePrintfMacros.h"

#include "jsapi.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsprf.h"
#include "jsscript.h"
#include "jsutil.h"
#include "NamespaceImports.h"

#include "gc/GCInternals.h"
#include "gc/Marking.h"
#include "gc/Zone.h"

#include "vm/Shape.h"
#include "vm/Symbol.h"

#include "jscompartmentinlines.h"
#include "jsgcinlines.h"

#include "vm/ObjectGroup-inl.h"

using namespace js;
using namespace js::gc;
using mozilla::DebugOnly;

namespace js {
template<typename T>
void
CheckTracedThing(JSTracer* trc, T thing);
} // namespace js


/*** Callback Tracer Dispatch ********************************************************************/

template <typename T>
T
DoCallback(JS::CallbackTracer* trc, T* thingp, const char* name)
{
    CheckTracedThing(trc, *thingp);
    JS::AutoTracingName ctx(trc, name);
    trc->dispatchToOnEdge(thingp);
    return *thingp;
}
#define INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS(name, type, _) \
    template type* DoCallback<type*>(JS::CallbackTracer*, type**, const char*);
JS_FOR_EACH_TRACEKIND(INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS);
#undef INSTANTIATE_ALL_VALID_TRACE_FUNCTIONS

template <typename S>
struct DoCallbackFunctor : public IdentityDefaultAdaptor<S> {
    template <typename T> S operator()(T* t, JS::CallbackTracer* trc, const char* name) {
        return js::gc::RewrapTaggedPointer<S, T*>::wrap(DoCallback(trc, &t, name));
    }
};

template <>
Value
DoCallback<Value>(JS::CallbackTracer* trc, Value* vp, const char* name)
{
    *vp = DispatchTyped(DoCallbackFunctor<Value>(), *vp, trc, name);
    return *vp;
}

template <>
jsid
DoCallback<jsid>(JS::CallbackTracer* trc, jsid* idp, const char* name)
{
    *idp = DispatchTyped(DoCallbackFunctor<jsid>(), *idp, trc, name);
    return *idp;
}

template <>
TaggedProto
DoCallback<TaggedProto>(JS::CallbackTracer* trc, TaggedProto* protop, const char* name)
{
    *protop = DispatchTyped(DoCallbackFunctor<TaggedProto>(), *protop, trc, name);
    return *protop;
}

void
JS::CallbackTracer::getTracingEdgeName(char* buffer, size_t bufferSize)
{
    MOZ_ASSERT(bufferSize > 0);
    if (contextFunctor_) {
        (*contextFunctor_)(this, buffer, bufferSize);
        return;
    }
    if (contextIndex_ != InvalidIndex) {
        JS_snprintf(buffer, bufferSize, "%s[%lu]", contextName_, contextIndex_);
        return;
    }
    JS_snprintf(buffer, bufferSize, "%s", contextName_);
}


/*** Public Tracing API **************************************************************************/

JS_PUBLIC_API(void)
JS_CallUnbarrieredValueTracer(JSTracer* trc, Value* valuep, const char* name)
{
    TraceManuallyBarrieredEdge(trc, valuep, name);
}

JS_PUBLIC_API(void)
JS_CallUnbarrieredIdTracer(JSTracer* trc, jsid* idp, const char* name)
{
    TraceManuallyBarrieredEdge(trc, idp, name);
}

JS_PUBLIC_API(void)
JS_CallUnbarrieredObjectTracer(JSTracer* trc, JSObject** objp, const char* name)
{
    TraceManuallyBarrieredEdge(trc, objp, name);
}

JS_PUBLIC_API(void)
JS_CallUnbarrieredStringTracer(JSTracer* trc, JSString** strp, const char* name)
{
    TraceManuallyBarrieredEdge(trc, strp, name);
}

JS_PUBLIC_API(void)
JS_CallUnbarrieredScriptTracer(JSTracer* trc, JSScript** scriptp, const char* name)
{
    TraceManuallyBarrieredEdge(trc, scriptp, name);
}

JS_PUBLIC_API(void)
JS_CallValueTracer(JSTracer* trc, JS::Heap<JS::Value>* valuep, const char* name)
{
    TraceManuallyBarrieredEdge(trc, valuep->unsafeGet(), name);
}

JS_PUBLIC_API(void)
JS_CallIdTracer(JSTracer* trc, JS::Heap<jsid>* idp, const char* name)
{
    TraceManuallyBarrieredEdge(trc, idp->unsafeGet(), name);
}

JS_PUBLIC_API(void)
JS_CallObjectTracer(JSTracer* trc, JS::Heap<JSObject*>* objp, const char* name)
{
    TraceManuallyBarrieredEdge(trc, objp->unsafeGet(), name);
}

JS_PUBLIC_API(void)
JS_CallStringTracer(JSTracer* trc, JS::Heap<JSString*>* strp, const char* name)
{
    TraceManuallyBarrieredEdge(trc, strp->unsafeGet(), name);
}

JS_PUBLIC_API(void)
JS_CallScriptTracer(JSTracer* trc, JS::Heap<JSScript*>* scriptp, const char* name)
{
    TraceManuallyBarrieredEdge(trc, scriptp->unsafeGet(), name);
}

JS_PUBLIC_API(void)
JS_CallFunctionTracer(JSTracer* trc, JS::Heap<JSFunction*>* funp, const char* name)
{
    TraceManuallyBarrieredEdge(trc, funp->unsafeGet(), name);
}

JS_PUBLIC_API(void)
JS_CallTenuredObjectTracer(JSTracer* trc, JS::TenuredHeap<JSObject*>* objp, const char* name)
{
    JSObject* obj = objp->getPtr();
    if (!obj)
        return;

    TraceManuallyBarrieredEdge(trc, &obj, name);

    objp->setPtr(obj);
}

JS_PUBLIC_API(void)
JS::TraceChildren(JSTracer* trc, GCCellPtr thing)
{
    js::TraceChildren(trc, thing.asCell(), thing.kind());
}

struct TraceChildrenFunctor {
    template <typename T>
    void operator()(JSTracer* trc, void* thing) {
        static_cast<T*>(thing)->traceChildren(trc);
    }
};

void
js::TraceChildren(JSTracer* trc, void* thing, JS::TraceKind kind)
{
    MOZ_ASSERT(thing);
    TraceChildrenFunctor f;
    DispatchTraceKindTyped(f, kind, trc, thing);
}

JS_PUBLIC_API(void)
JS_TraceRuntime(JSTracer* trc)
{
    AssertHeapIsIdle(trc->runtime());
    TraceRuntime(trc);
}

JS_PUBLIC_API(void)
JS_TraceIncomingCCWs(JSTracer* trc, const JS::ZoneSet& zones)
{
    for (js::ZonesIter z(trc->runtime(), SkipAtoms); !z.done(); z.next()) {
        Zone* zone = z.get();
        if (!zone || zones.has(zone))
            continue;

        for (js::CompartmentsInZoneIter c(zone); !c.done(); c.next()) {
            JSCompartment* comp = c.get();
            if (!comp)
                continue;

            for (JSCompartment::WrapperEnum e(comp); !e.empty(); e.popFront()) {
                const CrossCompartmentKey& key = e.front().key();
                JSObject* obj;
                JSScript* script;

                switch (key.kind) {
                  case CrossCompartmentKey::StringWrapper:
                    // StringWrappers are just used to avoid copying strings
                    // across zones multiple times, and don't hold a strong
                    // reference.
                    continue;

                  case CrossCompartmentKey::ObjectWrapper:
                  case CrossCompartmentKey::DebuggerObject:
                  case CrossCompartmentKey::DebuggerSource:
                  case CrossCompartmentKey::DebuggerEnvironment:
                    obj = static_cast<JSObject*>(key.wrapped);
                    // Ignore CCWs whose wrapped value doesn't live in our given
                    // set of zones.
                    if (!zones.has(obj->zone()))
                        continue;

                    TraceManuallyBarrieredEdge(trc, &obj, "cross-compartment wrapper");
                    MOZ_ASSERT(obj == key.wrapped);
                    break;

                  case CrossCompartmentKey::DebuggerScript:
                    script = static_cast<JSScript*>(key.wrapped);
                    // Ignore CCWs whose wrapped value doesn't live in our given
                    // set of zones.
                    if (!zones.has(script->zone()))
                        continue;
                    TraceManuallyBarrieredEdge(trc, &script, "cross-compartment wrapper");
                    MOZ_ASSERT(script == key.wrapped);
                    break;
                }
            }
        }
    }
}


/*** Cycle Collector Helpers **********************************************************************/

// This function is used by the Cycle Collector (CC) to trace through -- or in
// CC parlance, traverse -- a Shape tree. The CC does not care about Shapes or
// BaseShapes, only the JSObjects held live by them. Thus, we walk the Shape
// lineage, but only report non-Shape things. This effectively makes the entire
// shape lineage into a single node in the CC, saving tremendous amounts of
// space and time in its algorithms.
//
// The algorithm implemented here uses only bounded stack space. This would be
// possible to implement outside the engine, but would require much extra
// infrastructure and many, many more slow GOT lookups. We have implemented it
// inside SpiderMonkey, despite the lack of general applicability, for the
// simplicity and performance of FireFox's embedding of this engine.
void
gc::TraceCycleCollectorChildren(JS::CallbackTracer* trc, Shape* shape)
{
    // We need to mark the global, but it's OK to only do this once instead of
    // doing it for every Shape in our lineage, since it's always the same
    // global.
    JSObject* global = shape->compartment()->unsafeUnbarrieredMaybeGlobal();
    MOZ_ASSERT(global);
    DoCallback(trc, &global, "global");

    do {
        MOZ_ASSERT(global == shape->compartment()->unsafeUnbarrieredMaybeGlobal());

        MOZ_ASSERT(shape->base());
        shape->base()->assertConsistency();

        TraceEdge(trc, &shape->propidRef(), "propid");

        if (shape->hasGetterObject()) {
            JSObject* tmp = shape->getterObject();
            DoCallback(trc, &tmp, "getter");
            MOZ_ASSERT(tmp == shape->getterObject());
        }

        if (shape->hasSetterObject()) {
            JSObject* tmp = shape->setterObject();
            DoCallback(trc, &tmp, "setter");
            MOZ_ASSERT(tmp == shape->setterObject());
        }

        shape = shape->previous();
    } while (shape);
}

void
TraceObjectGroupCycleCollectorChildrenCallback(JS::CallbackTracer* trc,
                                               void** thingp, JS::TraceKind kind);

// Object groups can point to other object groups via an UnboxedLayout or the
// the original unboxed group link. There can potentially be deep or cyclic
// chains of such groups to trace through without going through a thing that
// participates in cycle collection. These need to be handled iteratively to
// avoid blowing the stack when running the cycle collector's callback tracer.
struct ObjectGroupCycleCollectorTracer : public JS::CallbackTracer
{
    explicit ObjectGroupCycleCollectorTracer(JS::CallbackTracer* innerTracer)
        : JS::CallbackTracer(innerTracer->runtime(), DoNotTraceWeakMaps),
          innerTracer(innerTracer)
    {}

    void onChild(const JS::GCCellPtr& thing) override;

    JS::CallbackTracer* innerTracer;
    Vector<ObjectGroup*, 4, SystemAllocPolicy> seen, worklist;
};

void
ObjectGroupCycleCollectorTracer::onChild(const JS::GCCellPtr& thing)
{
    if (thing.is<JSObject>() || thing.is<JSScript>()) {
        // Invoke the inner cycle collector callback on this child. It will not
        // recurse back into TraceChildren.
        innerTracer->onChild(thing);
        return;
    }

    if (thing.is<ObjectGroup>()) {
        // If this group is required to be in an ObjectGroup chain, trace it
        // via the provided worklist rather than continuing to recurse.
        ObjectGroup& group = thing.as<ObjectGroup>();
        if (group.maybeUnboxedLayout()) {
            for (size_t i = 0; i < seen.length(); i++) {
                if (seen[i] == &group)
                    return;
            }
            if (seen.append(&group) && worklist.append(&group)) {
                return;
            } else {
                // If append fails, keep tracing normally. The worst that will
                // happen is we end up overrecursing.
            }
        }
    }

    TraceChildren(this, thing.asCell(), thing.kind());
}

void
gc::TraceCycleCollectorChildren(JS::CallbackTracer* trc, ObjectGroup* group)
{
    MOZ_ASSERT(trc->isCallbackTracer());

    // Early return if this group is not required to be in an ObjectGroup chain.
    if (!group->maybeUnboxedLayout())
        return group->traceChildren(trc);

    ObjectGroupCycleCollectorTracer groupTracer(trc->asCallbackTracer());
    group->traceChildren(&groupTracer);

    while (!groupTracer.worklist.empty()) {
        ObjectGroup* innerGroup = groupTracer.worklist.popCopy();
        innerGroup->traceChildren(&groupTracer);
    }
}


/*** Traced Edge Printer *************************************************************************/

static size_t
CountDecimalDigits(size_t num)
{
    size_t numDigits = 0;
    do {
        num /= 10;
        numDigits++;
    } while (num > 0);

    return numDigits;
}

JS_PUBLIC_API(void)
JS_GetTraceThingInfo(char* buf, size_t bufsize, JSTracer* trc, void* thing,
                     JS::TraceKind kind, bool details)
{
    const char* name = nullptr; /* silence uninitialized warning */
    size_t n;

    if (bufsize == 0)
        return;

    switch (kind) {
      case JS::TraceKind::Object:
      {
        name = static_cast<JSObject*>(thing)->getClass()->name;
        break;
      }

      case JS::TraceKind::Script:
        name = "script";
        break;

      case JS::TraceKind::String:
        name = ((JSString*)thing)->isDependent()
               ? "substring"
               : "string";
        break;

      case JS::TraceKind::Symbol:
        name = "symbol";
        break;

      case JS::TraceKind::BaseShape:
        name = "base_shape";
        break;

      case JS::TraceKind::JitCode:
        name = "jitcode";
        break;

      case JS::TraceKind::LazyScript:
        name = "lazyscript";
        break;

      case JS::TraceKind::Shape:
        name = "shape";
        break;

      case JS::TraceKind::ObjectGroup:
        name = "object_group";
        break;

      default:
        name = "INVALID";
        break;
    }

    n = strlen(name);
    if (n > bufsize - 1)
        n = bufsize - 1;
    js_memcpy(buf, name, n + 1);
    buf += n;
    bufsize -= n;
    *buf = '\0';

    if (details && bufsize > 2) {
        switch (kind) {
          case JS::TraceKind::Object:
          {
            JSObject* obj = (JSObject*)thing;
            if (obj->is<JSFunction>()) {
                JSFunction* fun = &obj->as<JSFunction>();
                if (fun->displayAtom()) {
                    *buf++ = ' ';
                    bufsize--;
                    PutEscapedString(buf, bufsize, fun->displayAtom(), 0);
                }
            } else if (obj->getClass()->flags & JSCLASS_HAS_PRIVATE) {
                JS_snprintf(buf, bufsize, " %p", obj->as<NativeObject>().getPrivate());
            } else {
                JS_snprintf(buf, bufsize, " <no private>");
            }
            break;
          }

          case JS::TraceKind::Script:
          {
            JSScript* script = static_cast<JSScript*>(thing);
            JS_snprintf(buf, bufsize, " %s:%" PRIuSIZE, script->filename(), script->lineno());
            break;
          }

          case JS::TraceKind::String:
          {
            *buf++ = ' ';
            bufsize--;
            JSString* str = (JSString*)thing;

            if (str->isLinear()) {
                bool willFit = str->length() + strlen("<length > ") +
                               CountDecimalDigits(str->length()) < bufsize;

                n = JS_snprintf(buf, bufsize, "<length %d%s> ",
                                (int)str->length(),
                                willFit ? "" : " (truncated)");
                buf += n;
                bufsize -= n;

                PutEscapedString(buf, bufsize, &str->asLinear(), 0);
            } else {
                JS_snprintf(buf, bufsize, "<rope: length %d>", (int)str->length());
            }
            break;
          }

          case JS::TraceKind::Symbol:
          {
            JS::Symbol* sym = static_cast<JS::Symbol*>(thing);
            if (JSString* desc = sym->description()) {
                if (desc->isLinear()) {
                    *buf++ = ' ';
                    bufsize--;
                    PutEscapedString(buf, bufsize, &desc->asLinear(), 0);
                } else {
                    JS_snprintf(buf, bufsize, "<nonlinear desc>");
                }
            } else {
                JS_snprintf(buf, bufsize, "<null>");
            }
            break;
          }

          default:
            break;
        }
    }
    buf[bufsize - 1] = '\0';
}

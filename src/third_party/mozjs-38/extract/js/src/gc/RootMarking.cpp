/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ArrayUtils.h"

#ifdef MOZ_VALGRIND
# include <valgrind/memcheck.h>
#endif

#include "jscntxt.h"
#include "jsgc.h"
#include "jsprf.h"
#include "jstypes.h"
#include "jswatchpoint.h"

#include "builtin/MapObject.h"
#include "frontend/BytecodeCompiler.h"
#include "gc/GCInternals.h"
#include "gc/Marking.h"
#include "jit/MacroAssembler.h"
#include "js/HashTable.h"
#include "vm/Debugger.h"
#include "vm/JSONParser.h"
#include "vm/PropDesc.h"

#include "jsgcinlines.h"
#include "jsobjinlines.h"

using namespace js;
using namespace js::gc;

using mozilla::ArrayEnd;

using JS::AutoGCRooter;

typedef RootedValueMap::Range RootRange;
typedef RootedValueMap::Entry RootEntry;
typedef RootedValueMap::Enum RootEnum;

// Note: the following two functions cannot be static as long as we are using
// GCC 4.4, since it requires template function parameters to have external
// linkage.

void
MarkBindingsRoot(JSTracer* trc, Bindings* bindings, const char* name)
{
    bindings->trace(trc);
}

void
MarkPropertyDescriptorRoot(JSTracer* trc, JSPropertyDescriptor* pd, const char* name)
{
    pd->trace(trc);
}

void
MarkPropDescRoot(JSTracer* trc, PropDesc* pd, const char* name)
{
    pd->trace(trc);
}

template <class T>
static inline bool
IgnoreExactRoot(T* thingp)
{
    return false;
}

template <class T>
inline bool
IgnoreExactRoot(T** thingp)
{
    return IsNullTaggedPointer(*thingp);
}

template <>
inline bool
IgnoreExactRoot(JSObject** thingp)
{
    return IsNullTaggedPointer(*thingp) || *thingp == TaggedProto::LazyProto;
}

template <class T, void MarkFunc(JSTracer* trc, T* ref, const char* name), class Source>
static inline void
MarkExactStackRootList(JSTracer* trc, Source* s, const char* name)
{
    Rooted<T>* rooter = s->template gcRooters<T>();
    while (rooter) {
        T* addr = rooter->address();
        if (!IgnoreExactRoot(addr))
            MarkFunc(trc, addr, name);
        rooter = rooter->previous();
    }
}

template<class T>
static void
MarkExactStackRootsAcrossTypes(T context, JSTracer* trc)
{
    MarkExactStackRootList<JSObject*, MarkObjectRoot>(trc, context, "exact-object");
    MarkExactStackRootList<Shape*, MarkShapeRoot>(trc, context, "exact-shape");
    MarkExactStackRootList<BaseShape*, MarkBaseShapeRoot>(trc, context, "exact-baseshape");
    MarkExactStackRootList<ObjectGroup*, MarkObjectGroupRoot>(
        trc, context, "exact-objectgroup");
    MarkExactStackRootList<JSString*, MarkStringRoot>(trc, context, "exact-string");
    MarkExactStackRootList<JS::Symbol*, MarkSymbolRoot>(trc, context, "exact-symbol");
    MarkExactStackRootList<jit::JitCode*, MarkJitCodeRoot>(trc, context, "exact-jitcode");
    MarkExactStackRootList<JSScript*, MarkScriptRoot>(trc, context, "exact-script");
    MarkExactStackRootList<LazyScript*, MarkLazyScriptRoot>(trc, context, "exact-lazy-script");
    MarkExactStackRootList<jsid, MarkIdRoot>(trc, context, "exact-id");
    MarkExactStackRootList<Value, MarkValueRoot>(trc, context, "exact-value");
    MarkExactStackRootList<TypeSet::Type, TypeSet::MarkTypeRoot>(trc, context, "TypeSet::Type");
    MarkExactStackRootList<Bindings, MarkBindingsRoot>(trc, context, "Bindings");
    MarkExactStackRootList<JSPropertyDescriptor, MarkPropertyDescriptorRoot>(
        trc, context, "JSPropertyDescriptor");
    MarkExactStackRootList<PropDesc, MarkPropDescRoot>(trc, context, "PropDesc");
}

static void
MarkExactStackRoots(JSRuntime* rt, JSTracer* trc)
{
    for (ContextIter cx(rt); !cx.done(); cx.next())
        MarkExactStackRootsAcrossTypes<JSContext*>(cx.get(), trc);
    MarkExactStackRootsAcrossTypes<PerThreadData*>(&rt->mainThread, trc);
}

void
JS::AutoIdArray::trace(JSTracer* trc)
{
    MOZ_ASSERT(tag_ == IDARRAY);
    gc::MarkIdRange(trc, idArray->length, idArray->vector, "JSAutoIdArray.idArray");
}

inline void
AutoGCRooter::trace(JSTracer* trc)
{
    switch (tag_) {
      case PARSER:
        frontend::MarkParser(trc, this);
        return;

      case IDARRAY: {
        JSIdArray* ida = static_cast<AutoIdArray*>(this)->idArray;
        MarkIdRange(trc, ida->length, ida->vector, "JS::AutoIdArray.idArray");
        return;
      }

      case DESCVECTOR: {
        AutoPropDescVector::VectorImpl& descriptors =
            static_cast<AutoPropDescVector*>(this)->vector;
        for (size_t i = 0, len = descriptors.length(); i < len; i++)
            descriptors[i].trace(trc);
        return;
      }

      case VALVECTOR: {
        AutoValueVector::VectorImpl& vector = static_cast<AutoValueVector*>(this)->vector;
        MarkValueRootRange(trc, vector.length(), vector.begin(), "js::AutoValueVector.vector");
        return;
      }

      case IDVECTOR: {
        AutoIdVector::VectorImpl& vector = static_cast<AutoIdVector*>(this)->vector;
        MarkIdRootRange(trc, vector.length(), vector.begin(), "js::AutoIdVector.vector");
        return;
      }

      case SHAPEVECTOR: {
        AutoShapeVector::VectorImpl& vector = static_cast<js::AutoShapeVector*>(this)->vector;
        MarkShapeRootRange(trc, vector.length(), const_cast<Shape**>(vector.begin()),
                           "js::AutoShapeVector.vector");
        return;
      }

      case OBJVECTOR: {
        AutoObjectVector::VectorImpl& vector = static_cast<AutoObjectVector*>(this)->vector;
        MarkObjectRootRange(trc, vector.length(), vector.begin(), "js::AutoObjectVector.vector");
        return;
      }

      case FUNVECTOR: {
        AutoFunctionVector::VectorImpl& vector = static_cast<AutoFunctionVector*>(this)->vector;
        MarkObjectRootRange(trc, vector.length(), vector.begin(), "js::AutoFunctionVector.vector");
        return;
      }

      case STRINGVECTOR: {
        AutoStringVector::VectorImpl& vector = static_cast<AutoStringVector*>(this)->vector;
        MarkStringRootRange(trc, vector.length(), vector.begin(), "js::AutoStringVector.vector");
        return;
      }

      case NAMEVECTOR: {
        AutoNameVector::VectorImpl& vector = static_cast<AutoNameVector*>(this)->vector;
        MarkStringRootRange(trc, vector.length(), vector.begin(), "js::AutoNameVector.vector");
        return;
      }

      case VALARRAY: {
        /*
         * We don't know the template size parameter, but we can safely treat it
         * as an AutoValueArray<1> because the length is stored separately.
         */
        AutoValueArray<1>* array = static_cast<AutoValueArray<1>*>(this);
        MarkValueRootRange(trc, array->length(), array->begin(), "js::AutoValueArray");
        return;
      }

      case SCRIPTVECTOR: {
        AutoScriptVector::VectorImpl& vector = static_cast<AutoScriptVector*>(this)->vector;
        MarkScriptRootRange(trc, vector.length(), vector.begin(), "js::AutoScriptVector.vector");
        return;
      }

      case OBJOBJHASHMAP: {
        AutoObjectObjectHashMap::HashMapImpl& map = static_cast<AutoObjectObjectHashMap*>(this)->map;
        for (AutoObjectObjectHashMap::Enum e(map); !e.empty(); e.popFront()) {
            MarkObjectRoot(trc, &e.front().value(), "AutoObjectObjectHashMap value");
            trc->setTracingLocation((void*)&e.front().key());
            JSObject* key = e.front().key();
            MarkObjectRoot(trc, &key, "AutoObjectObjectHashMap key");
            if (key != e.front().key())
                e.rekeyFront(key);
        }
        return;
      }

      case OBJU32HASHMAP: {
        AutoObjectUnsigned32HashMap* self = static_cast<AutoObjectUnsigned32HashMap*>(this);
        AutoObjectUnsigned32HashMap::HashMapImpl& map = self->map;
        for (AutoObjectUnsigned32HashMap::Enum e(map); !e.empty(); e.popFront()) {
            JSObject* key = e.front().key();
            MarkObjectRoot(trc, &key, "AutoObjectUnsignedHashMap key");
            if (key != e.front().key())
                e.rekeyFront(key);
        }
        return;
      }

      case OBJHASHSET: {
        AutoObjectHashSet* self = static_cast<AutoObjectHashSet*>(this);
        AutoObjectHashSet::HashSetImpl& set = self->set;
        for (AutoObjectHashSet::Enum e(set); !e.empty(); e.popFront()) {
            JSObject* obj = e.front();
            MarkObjectRoot(trc, &obj, "AutoObjectHashSet value");
            if (obj != e.front())
                e.rekeyFront(obj);
        }
        return;
      }

      case HASHABLEVALUE: {
        AutoHashableValueRooter* rooter = static_cast<AutoHashableValueRooter*>(this);
        rooter->trace(trc);
        return;
      }

      case IONMASM: {
        static_cast<js::jit::MacroAssembler::AutoRooter*>(this)->masm()->trace(trc);
        return;
      }

      case WRAPPER: {
        /*
         * We need to use MarkValueUnbarriered here because we mark wrapper
         * roots in every slice. This is because of some rule-breaking in
         * RemapAllWrappersForObject; see comment there.
         */
          MarkValueUnbarriered(trc, &static_cast<AutoWrapperRooter*>(this)->value.get(),
                               "JS::AutoWrapperRooter.value");
        return;
      }

      case WRAPVECTOR: {
        AutoWrapperVector::VectorImpl& vector = static_cast<AutoWrapperVector*>(this)->vector;
        /*
         * We need to use MarkValueUnbarriered here because we mark wrapper
         * roots in every slice. This is because of some rule-breaking in
         * RemapAllWrappersForObject; see comment there.
         */
        for (WrapperValue* p = vector.begin(); p < vector.end(); p++)
            MarkValueUnbarriered(trc, &p->get(), "js::AutoWrapperVector.vector");
        return;
      }

      case JSONPARSER:
        static_cast<js::JSONParserBase*>(this)->trace(trc);
        return;

      case CUSTOM:
        static_cast<JS::CustomAutoRooter*>(this)->trace(trc);
        return;
    }

    MOZ_ASSERT(tag_ >= 0);
    if (Value* vp = static_cast<AutoArrayRooter*>(this)->array)
        MarkValueRootRange(trc, tag_, vp, "JS::AutoArrayRooter.array");
}

/* static */ void
AutoGCRooter::traceAll(JSTracer* trc)
{
    for (ContextIter cx(trc->runtime()); !cx.done(); cx.next())
        traceAllInContext(&*cx, trc);
}

/* static */ void
AutoGCRooter::traceAllWrappers(JSTracer* trc)
{
    for (ContextIter cx(trc->runtime()); !cx.done(); cx.next()) {
        for (AutoGCRooter* gcr = cx->autoGCRooters; gcr; gcr = gcr->down) {
            if (gcr->tag_ == WRAPVECTOR || gcr->tag_ == WRAPPER)
                gcr->trace(trc);
        }
    }
}

void
AutoHashableValueRooter::trace(JSTracer* trc)
{
    MarkValueRoot(trc, reinterpret_cast<Value*>(&value), "AutoHashableValueRooter");
}

void
StackShape::trace(JSTracer* trc)
{
    if (base)
        MarkBaseShapeRoot(trc, (BaseShape**) &base, "StackShape base");

    MarkIdRoot(trc, (jsid*) &propid, "StackShape id");

    if ((attrs & JSPROP_GETTER) && rawGetter)
        MarkObjectRoot(trc, (JSObject**)&rawGetter, "StackShape getter");

    if ((attrs & JSPROP_SETTER) && rawSetter)
        MarkObjectRoot(trc, (JSObject**)&rawSetter, "StackShape setter");
}

void
JSPropertyDescriptor::trace(JSTracer* trc)
{
    if (obj)
        MarkObjectRoot(trc, &obj, "Descriptor::obj");
    MarkValueRoot(trc, &value, "Descriptor::value");
    if ((attrs & JSPROP_GETTER) && getter) {
        JSObject* tmp = JS_FUNC_TO_DATA_PTR(JSObject*, getter);
        MarkObjectRoot(trc, &tmp, "Descriptor::get");
        getter = JS_DATA_TO_FUNC_PTR(JSPropertyOp, tmp);
    }
    if ((attrs & JSPROP_SETTER) && setter) {
        JSObject* tmp = JS_FUNC_TO_DATA_PTR(JSObject*, setter);
        MarkObjectRoot(trc, &tmp, "Descriptor::set");
        setter = JS_DATA_TO_FUNC_PTR(JSStrictPropertyOp, tmp);
    }
}

namespace js {
namespace gc {

template<typename T>
struct PersistentRootedMarker
{
    typedef PersistentRooted<T> Element;
    typedef mozilla::LinkedList<Element> List;
    typedef void (*MarkFunc)(JSTracer* trc, T* ref, const char* name);

    template <MarkFunc Mark>
    static void
    markChainIfNotNull(JSTracer* trc, List& list, const char* name)
    {
        for (Element* r = list.getFirst(); r; r = r->getNext()) {
            if (r->get())
                Mark(trc, r->address(), name);
        }
    }

    template <MarkFunc Mark>
    static void
    markChain(JSTracer* trc, List& list, const char* name)
    {
        for (Element* r = list.getFirst(); r; r = r->getNext())
            Mark(trc, r->address(), name);
    }
};
}
}

void
js::gc::MarkPersistentRootedChains(JSTracer* trc)
{
    JSRuntime* rt = trc->runtime();

    // Mark the PersistentRooted chains of types that may be null.
    PersistentRootedMarker<JSFunction*>::markChainIfNotNull<MarkObjectRoot>(
        trc, rt->functionPersistentRooteds, "PersistentRooted<JSFunction*>");
    PersistentRootedMarker<JSObject*>::markChainIfNotNull<MarkObjectRoot>(
        trc, rt->objectPersistentRooteds, "PersistentRooted<JSObject*>");
    PersistentRootedMarker<JSScript*>::markChainIfNotNull<MarkScriptRoot>(
        trc, rt->scriptPersistentRooteds, "PersistentRooted<JSScript*>");
    PersistentRootedMarker<JSString*>::markChainIfNotNull<MarkStringRoot>(
        trc, rt->stringPersistentRooteds, "PersistentRooted<JSString*>");

    // Mark the PersistentRooted chains of types that are never null.
    PersistentRootedMarker<jsid>::markChain<MarkIdRoot>(trc, rt->idPersistentRooteds,
                                                        "PersistentRooted<jsid>");
    PersistentRootedMarker<Value>::markChain<MarkValueRoot>(trc, rt->valuePersistentRooteds,
                                                            "PersistentRooted<Value>");
}

void
js::gc::GCRuntime::markRuntime(JSTracer* trc,
                               TraceOrMarkRuntime traceOrMark,
                               TraceRootsOrUsedSaved rootsSource)
{
    gcstats::AutoPhase ap(stats, gcstats::PHASE_MARK_ROOTS);

    MOZ_ASSERT(trc->callback != GCMarker::GrayCallback);
    MOZ_ASSERT(traceOrMark == TraceRuntime || traceOrMark == MarkRuntime);
    MOZ_ASSERT(rootsSource == TraceRoots || rootsSource == UseSavedRoots);

    MOZ_ASSERT(!rt->mainThread.suppressGC);

    if (traceOrMark == MarkRuntime) {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_MARK_CCWS);

        for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
            if (!c->zone()->isCollecting())
                c->markCrossCompartmentWrappers(trc);
        }
        Debugger::markAllCrossCompartmentEdges(trc);
    }

    {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_MARK_ROOTERS);

        AutoGCRooter::traceAll(trc);

        if (!rt->isBeingDestroyed()) {
            MarkExactStackRoots(rt, trc);
            rt->markSelfHostingGlobal(trc);
        }

        for (RootRange r = rootsHash.all(); !r.empty(); r.popFront()) {
            const RootEntry& entry = r.front();
            MarkValueRoot(trc, entry.key(), entry.value());
        }

        MarkPersistentRootedChains(trc);
    }

    if (rt->scriptAndCountsVector) {
        ScriptAndCountsVector& vec = *rt->scriptAndCountsVector;
        for (size_t i = 0; i < vec.length(); i++)
            MarkScriptRoot(trc, &vec[i].script, "scriptAndCountsVector");
    }

    if (!rt->isBeingDestroyed() && !trc->runtime()->isHeapMinorCollecting()) {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_MARK_RUNTIME_DATA);

        if (traceOrMark == TraceRuntime || rt->atomsCompartment()->zone()->isCollecting()) {
            MarkPermanentAtoms(trc);
            MarkAtoms(trc);
            MarkWellKnownSymbols(trc);
            jit::JitRuntime::Mark(trc);
        }
    }

    for (ContextIter acx(rt); !acx.done(); acx.next())
        acx->mark(trc);

    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        if (traceOrMark == MarkRuntime && !zone->isCollecting())
            continue;

        /* Do not discard scripts with counts while profiling. */
        if (rt->profilingScripts && !isHeapMinorCollecting()) {
            for (ZoneCellIterUnderGC i(zone, FINALIZE_SCRIPT); !i.done(); i.next()) {
                JSScript* script = i.get<JSScript>();
                if (script->hasScriptCounts()) {
                    MarkScriptRoot(trc, &script, "profilingScripts");
                    MOZ_ASSERT(script == i.get<JSScript>());
                }
            }
        }
    }

    /* We can't use GCCompartmentsIter if we're called from TraceRuntime. */
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        if (trc->runtime()->isHeapMinorCollecting())
            c->globalWriteBarriered = false;

        if (traceOrMark == MarkRuntime && !c->zone()->isCollecting())
            continue;

        /* During a GC, these are treated as weak pointers. */
        if (traceOrMark == TraceRuntime) {
            if (c->watchpointMap)
                c->watchpointMap->markAll(trc);
        }

        /* Mark debug scopes, if present */
        if (c->debugScopes)
            c->debugScopes->mark(trc);

        if (c->lazyArrayBuffers)
            c->lazyArrayBuffers->trace(trc);
    }

    MarkInterpreterActivations(rt, trc);

    jit::MarkJitActivations(rt, trc);

    if (!isHeapMinorCollecting()) {
        gcstats::AutoPhase ap(stats, gcstats::PHASE_MARK_EMBEDDING);

        /*
         * All JSCompartment::markRoots() does is mark the globals for
         * compartments which have been entered. Globals aren't nursery
         * allocated so there's no need to do this for minor GCs.
         */
        for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next())
            c->markRoots(trc);

        /*
         * The embedding can register additional roots here.
         *
         * We don't need to trace these in a minor GC because all pointers into
         * the nursery should be in the store buffer, and we want to avoid the
         * time taken to trace all these roots.
         */
        for (size_t i = 0; i < blackRootTracers.length(); i++) {
            const Callback<JSTraceDataOp>& e = blackRootTracers[i];
            (*e.op)(trc, e.data);
        }

        /* During GC, we don't mark gray roots at this stage. */
        if (JSTraceDataOp op = grayRootTracer.op) {
            if (traceOrMark == TraceRuntime)
                (*op)(trc, grayRootTracer.data);
        }
    }
}

void
js::gc::GCRuntime::bufferGrayRoots()
{
    marker.startBufferingGrayRoots();
    if (JSTraceDataOp op = grayRootTracer.op)
        (*op)(&marker, grayRootTracer.data);
    marker.endBufferingGrayRoots();
}

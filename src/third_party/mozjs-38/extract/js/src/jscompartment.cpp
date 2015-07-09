/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscompartmentinlines.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"

#include "jscntxt.h"
#include "jsfriendapi.h"
#include "jsgc.h"
#include "jsiter.h"
#include "jswatchpoint.h"
#include "jswrapper.h"

#include "gc/Marking.h"
#include "jit/JitCompartment.h"
#include "js/Proxy.h"
#include "js/RootingAPI.h"
#include "proxy/DeadObjectProxy.h"
#include "vm/Debugger.h"
#include "vm/StopIterationObject.h"
#include "vm/WrapperObject.h"

#include "jsatominlines.h"
#include "jsfuninlines.h"
#include "jsgcinlines.h"
#include "jsobjinlines.h"

using namespace js;
using namespace js::gc;
using namespace js::jit;

using mozilla::DebugOnly;

JSCompartment::JSCompartment(Zone* zone, const JS::CompartmentOptions& options = JS::CompartmentOptions())
  : options_(options),
    zone_(zone),
    runtime_(zone->runtimeFromMainThread()),
    principals(nullptr),
    isSystem(false),
    isSelfHosting(false),
    marked(true),
    addonId(options.addonIdOrNull()),
#ifdef DEBUG
    firedOnNewGlobalObject(false),
#endif
    global_(nullptr),
    enterCompartmentDepth(0),
    totalTime(0),
    data(nullptr),
    objectMetadataCallback(nullptr),
    lastAnimationTime(0),
    regExps(runtime_),
    globalWriteBarriered(false),
    neuteredTypedObjects(0),
    propertyTree(thisForCtor()),
    selfHostingScriptSource(nullptr),
    lazyArrayBuffers(nullptr),
    gcIncomingGrayPointers(nullptr),
    gcWeakMapList(nullptr),
    gcPreserveJitCode(options.preserveJitCode()),
    debugModeBits(0),
    rngState(0),
    watchpointMap(nullptr),
    scriptCountsMap(nullptr),
    debugScriptMap(nullptr),
    debugScopes(nullptr),
    enumerators(nullptr),
    compartmentStats(nullptr),
    scheduledForDestruction(false),
    maybeAlive(true),
    jitCompartment_(nullptr)
{
    PodArrayZero(sawDeprecatedLanguageExtension);
    runtime_->numCompartments++;
    MOZ_ASSERT_IF(options.mergeable(), options.invisibleToDebugger());
}

JSCompartment::~JSCompartment()
{
    reportTelemetry();

    js_delete(jitCompartment_);
    js_delete(watchpointMap);
    js_delete(scriptCountsMap);
    js_delete(debugScriptMap);
    js_delete(debugScopes);
    js_delete(lazyArrayBuffers);
    js_free(enumerators);

    runtime_->numCompartments--;
}

bool
JSCompartment::init(JSContext* cx)
{
    /*
     * As a hack, we clear our timezone cache every time we create a new
     * compartment. This ensures that the cache is always relatively fresh, but
     * shouldn't interfere with benchmarks which create tons of date objects
     * (unless they also create tons of iframes, which seems unlikely).
     */
    if (cx)
        cx->runtime()->dateTimeInfo.updateTimeZoneAdjustment();

    if (!crossCompartmentWrappers.init(0))
        return false;

    if (!regExps.init(cx))
        return false;

    enumerators = NativeIterator::allocateSentinel(cx);
    if (!enumerators)
        return false;

    if (!savedStacks_.init())
        return false;

    return true;
}

jit::JitRuntime*
JSRuntime::createJitRuntime(JSContext* cx)
{
    // The shared stubs are created in the atoms compartment, which may be
    // accessed by other threads with an exclusive context.
    AutoLockForExclusiveAccess atomsLock(cx);

    MOZ_ASSERT(!jitRuntime_);

    jit::JitRuntime* jrt = cx->new_<jit::JitRuntime>();
    if (!jrt)
        return nullptr;

    // Protect jitRuntime_ from being observed (by InterruptRunningJitCode)
    // while it is being initialized. Unfortunately, initialization depends on
    // jitRuntime_ being non-null, so we can't just wait to assign jitRuntime_.
    JitRuntime::AutoMutateBackedges amb(jrt);
    jitRuntime_ = jrt;

    if (!jitRuntime_->initialize(cx)) {
        js_ReportOutOfMemory(cx);

        js_delete(jitRuntime_);
        jitRuntime_ = nullptr;

        JSCompartment* comp = cx->runtime()->atomsCompartment();
        if (comp->jitCompartment_) {
            js_delete(comp->jitCompartment_);
            comp->jitCompartment_ = nullptr;
        }

        return nullptr;
    }

    return jitRuntime_;
}

bool
JSCompartment::ensureJitCompartmentExists(JSContext* cx)
{
    using namespace js::jit;
    if (jitCompartment_)
        return true;

    if (!zone()->getJitZone(cx))
        return false;

    /* Set the compartment early, so linking works. */
    jitCompartment_ = cx->new_<JitCompartment>();

    if (!jitCompartment_)
        return false;

    if (!jitCompartment_->initialize(cx)) {
        js_delete(jitCompartment_);
        jitCompartment_ = nullptr;
        return false;
    }

    return true;
}

/*
 * This class is used to add a post barrier on the crossCompartmentWrappers map,
 * as the key is calculated based on objects which may be moved by generational
 * GC.
 */
class WrapperMapRef : public BufferableRef
{
    WrapperMap* map;
    CrossCompartmentKey key;

  public:
    WrapperMapRef(WrapperMap* map, const CrossCompartmentKey& key)
      : map(map), key(key) {}

    void mark(JSTracer* trc) {
        CrossCompartmentKey prior = key;
        if (key.debugger)
            Mark(trc, &key.debugger, "CCW debugger");
        if (key.kind != CrossCompartmentKey::StringWrapper)
            Mark(trc, reinterpret_cast<JSObject**>(&key.wrapped), "CCW wrapped object");
        if (key.debugger == prior.debugger && key.wrapped == prior.wrapped)
            return;

        /* Look for the original entry, which might have been removed. */
        WrapperMap::Ptr p = map->lookup(prior);
        if (!p)
            return;

        /* Rekey the entry. */
        map->rekeyAs(prior, key, key);
    }
};

#ifdef JSGC_HASH_TABLE_CHECKS
void
JSCompartment::checkWrapperMapAfterMovingGC()
{
    /*
     * Assert that the postbarriers have worked and that nothing is left in
     * wrapperMap that points into the nursery, and that the hash table entries
     * are discoverable.
     */
    for (WrapperMap::Enum e(crossCompartmentWrappers); !e.empty(); e.popFront()) {
        CrossCompartmentKey key = e.front().key();
        CheckGCThingAfterMovingGC(key.debugger);
        CheckGCThingAfterMovingGC(key.wrapped);
        CheckGCThingAfterMovingGC(static_cast<Cell*>(e.front().value().get().toGCThing()));

        WrapperMap::Ptr ptr = crossCompartmentWrappers.lookup(key);
        MOZ_ASSERT(ptr.found() && &*ptr == &e.front());
    }
}
#endif

bool
JSCompartment::putWrapper(JSContext* cx, const CrossCompartmentKey& wrapped, const js::Value& wrapper)
{
    MOZ_ASSERT(wrapped.wrapped);
    MOZ_ASSERT(!IsPoisonedPtr(wrapped.wrapped));
    MOZ_ASSERT(!IsPoisonedPtr(wrapped.debugger));
    MOZ_ASSERT(!IsPoisonedPtr(wrapper.toGCThing()));
    MOZ_ASSERT_IF(wrapped.kind == CrossCompartmentKey::StringWrapper, wrapper.isString());
    MOZ_ASSERT_IF(wrapped.kind != CrossCompartmentKey::StringWrapper, wrapper.isObject());
    bool success = crossCompartmentWrappers.put(wrapped, ReadBarriered<Value>(wrapper));

    /* There's no point allocating wrappers in the nursery since we will tenure them anyway. */
    MOZ_ASSERT(!IsInsideNursery(static_cast<gc::Cell*>(wrapper.toGCThing())));

    if (success && (IsInsideNursery(wrapped.wrapped) || IsInsideNursery(wrapped.debugger))) {
        WrapperMapRef ref(&crossCompartmentWrappers, wrapped);
        cx->runtime()->gc.storeBuffer.putGeneric(ref);
    }

    return success;
}

static JSString*
CopyStringPure(JSContext* cx, JSString* str)
{
    /*
     * Directly allocate the copy in the destination compartment, rather than
     * first flattening it (and possibly allocating in source compartment),
     * because we don't know whether the flattening will pay off later.
     */

    size_t len = str->length();
    JSString* copy;
    if (str->isLinear()) {
        /* Only use AutoStableStringChars if the NoGC allocation fails. */
        if (str->hasLatin1Chars()) {
            JS::AutoCheckCannotGC nogc;
            copy = NewStringCopyN<NoGC>(cx, str->asLinear().latin1Chars(nogc), len);
        } else {
            JS::AutoCheckCannotGC nogc;
            copy = NewStringCopyNDontDeflate<NoGC>(cx, str->asLinear().twoByteChars(nogc), len);
        }
        if (copy)
            return copy;

        AutoStableStringChars chars(cx);
        if (!chars.init(cx, str))
            return nullptr;

        return chars.isLatin1()
               ? NewStringCopyN<CanGC>(cx, chars.latin1Range().start().get(), len)
               : NewStringCopyNDontDeflate<CanGC>(cx, chars.twoByteRange().start().get(), len);
    }

    if (str->hasLatin1Chars()) {
        ScopedJSFreePtr<Latin1Char> copiedChars;
        if (!str->asRope().copyLatin1CharsZ(cx, copiedChars))
            return nullptr;

        return NewString<CanGC>(cx, copiedChars.forget(), len);
    }

    ScopedJSFreePtr<char16_t> copiedChars;
    if (!str->asRope().copyTwoByteCharsZ(cx, copiedChars))
        return nullptr;

    return NewStringDontDeflate<CanGC>(cx, copiedChars.forget(), len);
}

bool
JSCompartment::wrap(JSContext* cx, MutableHandleString strp)
{
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(this));
    MOZ_ASSERT(cx->compartment() == this);

    /* If the string is already in this compartment, we are done. */
    JSString* str = strp;
    if (str->zoneFromAnyThread() == zone())
        return true;

    /* If the string is an atom, we don't have to copy. */
    if (str->isAtom()) {
        MOZ_ASSERT(str->isPermanentAtom() ||
                   cx->runtime()->isAtomsZone(str->zone()));
        return true;
    }

    /* Check the cache. */
    RootedValue key(cx, StringValue(str));
    if (WrapperMap::Ptr p = crossCompartmentWrappers.lookup(CrossCompartmentKey(key))) {
        strp.set(p->value().get().toString());
        return true;
    }

    /* No dice. Make a copy, and cache it. */
    JSString* copy = CopyStringPure(cx, str);
    if (!copy)
        return false;
    if (!putWrapper(cx, CrossCompartmentKey(key), StringValue(copy)))
        return false;

    strp.set(copy);
    return true;
}

bool
JSCompartment::wrap(JSContext* cx, MutableHandleObject obj, HandleObject existingArg)
{
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(this));
    MOZ_ASSERT(cx->compartment() == this);
    MOZ_ASSERT_IF(existingArg, existingArg->compartment() == cx->compartment());
    MOZ_ASSERT_IF(existingArg, IsDeadProxyObject(existingArg));

    if (!obj)
        return true;
    AutoDisableProxyCheck adpc(cx->runtime());

    // Wrappers should really be parented to the wrapped parent of the wrapped
    // object, but in that case a wrapped global object would have a nullptr
    // parent without being a proper global object (JSCLASS_IS_GLOBAL). Instead,
    // we parent all wrappers to the global object in their home compartment.
    // This loses us some transparency, and is generally very cheesy.
    HandleObject global = cx->global();
    RootedObject objGlobal(cx, &obj->global());
    MOZ_ASSERT(global);
    MOZ_ASSERT(objGlobal);

    const JSWrapObjectCallbacks* cb = cx->runtime()->wrapObjectCallbacks;

    if (obj->compartment() == this) {
        obj.set(GetOuterObject(cx, obj));
        return true;
    }

    // If we have a cross-compartment wrapper, make sure that the cx isn't
    // associated with the self-hosting global. We don't want to create
    // wrappers for objects in other runtimes, which may be the case for the
    // self-hosting global.
    MOZ_ASSERT(!cx->runtime()->isSelfHostingGlobal(global) &&
               !cx->runtime()->isSelfHostingGlobal(objGlobal));

    // Unwrap the object, but don't unwrap outer windows.
    RootedObject objectPassedToWrap(cx, obj);
    obj.set(UncheckedUnwrap(obj, /* stopAtOuter = */ true));

    if (obj->compartment() == this) {
        MOZ_ASSERT(obj == GetOuterObject(cx, obj));
        return true;
    }

    // Translate StopIteration singleton.
    if (obj->is<StopIterationObject>()) {
        // StopIteration isn't a constructor, but it's stored in GlobalObject
        // as one, out of laziness. Hence the GetBuiltinConstructor call here.
        RootedObject stopIteration(cx);
        if (!GetBuiltinConstructor(cx, JSProto_StopIteration, &stopIteration))
            return false;
        obj.set(stopIteration);
        return true;
    }

    // Invoke the prewrap callback. We're a bit worried about infinite
    // recursion here, so we do a check - see bug 809295.
    JS_CHECK_SYSTEM_RECURSION(cx, return false);
    if (cb->preWrap) {
        obj.set(cb->preWrap(cx, global, obj, objectPassedToWrap));
        if (!obj)
            return false;
    }
    MOZ_ASSERT(obj == GetOuterObject(cx, obj));

    if (obj->compartment() == this)
        return true;


    // If we already have a wrapper for this value, use it.
    RootedValue key(cx, ObjectValue(*obj));
    if (WrapperMap::Ptr p = crossCompartmentWrappers.lookup(CrossCompartmentKey(key))) {
        obj.set(&p->value().get().toObject());
        MOZ_ASSERT(obj->is<CrossCompartmentWrapperObject>());
        MOZ_ASSERT(obj->getParent() == global);
        return true;
    }

    RootedObject existing(cx, existingArg);
    if (existing) {
        // Is it possible to reuse |existing|?
        if (!existing->getTaggedProto().isLazy() ||
            // Note: Class asserted above, so all that's left to check is callability
            existing->isCallable() ||
            existing->getParent() != global ||
            obj->isCallable())
        {
            existing = nullptr;
        }
    }

    obj.set(cb->wrap(cx, existing, obj, global));
    if (!obj)
        return false;

    // We maintain the invariant that the key in the cross-compartment wrapper
    // map is always directly wrapped by the value.
    MOZ_ASSERT(Wrapper::wrappedObject(obj) == &key.get().toObject());

    return putWrapper(cx, CrossCompartmentKey(key), ObjectValue(*obj));
}

bool
JSCompartment::wrap(JSContext* cx, MutableHandle<PropertyDescriptor> desc)
{
    if (!wrap(cx, desc.object()))
        return false;

    if (desc.hasGetterObject()) {
        if (!wrap(cx, desc.getterObject()))
            return false;
    }
    if (desc.hasSetterObject()) {
        if (!wrap(cx, desc.setterObject()))
            return false;
    }

    return wrap(cx, desc.value());
}

bool
JSCompartment::wrap(JSContext* cx, MutableHandle<PropDesc> desc)
{
    if (desc.isUndefined())
        return true;

    JSCompartment* comp = cx->compartment();

    if (desc.hasValue()) {
        RootedValue value(cx, desc.value());
        if (!comp->wrap(cx, &value))
            return false;
        desc.setValue(value);
    }
    if (desc.hasGet()) {
        RootedValue get(cx, desc.getterValue());
        if (!comp->wrap(cx, &get))
            return false;
        desc.setGetter(get);
    }
    if (desc.hasSet()) {
        RootedValue set(cx, desc.setterValue());
        if (!comp->wrap(cx, &set))
            return false;
        desc.setSetter(set);
    }
    return true;
}

/*
 * This method marks pointers that cross compartment boundaries. It should be
 * called only for per-compartment GCs, since full GCs naturally follow pointers
 * across compartments.
 */
void
JSCompartment::markCrossCompartmentWrappers(JSTracer* trc)
{
    MOZ_ASSERT(!zone()->isCollecting());

    for (WrapperMap::Enum e(crossCompartmentWrappers); !e.empty(); e.popFront()) {
        Value v = e.front().value();
        if (e.front().key().kind == CrossCompartmentKey::ObjectWrapper) {
            ProxyObject* wrapper = &v.toObject().as<ProxyObject>();

            /*
             * We have a cross-compartment wrapper. Its private pointer may
             * point into the compartment being collected, so we should mark it.
             */
            MarkValue(trc, wrapper->slotOfPrivate(), "cross-compartment wrapper");
        }
    }
}

void
JSCompartment::trace(JSTracer* trc)
{
    savedStacks_.trace(trc);
}

void
JSCompartment::markRoots(JSTracer* trc)
{
    MOZ_ASSERT(!trc->runtime()->isHeapMinorCollecting());

    if (jitCompartment_)
        jitCompartment_->mark(trc, this);

    /*
     * If a compartment is on-stack, we mark its global so that
     * JSContext::global() remains valid.
     */
    if (enterCompartmentDepth && global_.unbarrieredGet())
        MarkObjectRoot(trc, global_.unsafeGet(), "on-stack compartment global");
}

void
JSCompartment::sweepInnerViews()
{
    innerViews.sweep(runtimeFromAnyThread());
}

void
JSCompartment::sweepSavedStacks()
{
    savedStacks_.sweep(runtimeFromAnyThread());
}

void
JSCompartment::sweepGlobalObject(FreeOp* fop)
{
    if (global_.unbarrieredGet() && IsObjectAboutToBeFinalizedFromAnyThread(global_.unsafeGet())) {
        if (isDebuggee())
            Debugger::detachAllDebuggersFromGlobal(fop, global_);
        global_.set(nullptr);
    }
}

void
JSCompartment::sweepSelfHostingScriptSource()
{
    if (selfHostingScriptSource.unbarrieredGet() &&
        IsObjectAboutToBeFinalizedFromAnyThread((JSObject**) selfHostingScriptSource.unsafeGet()))
    {
        selfHostingScriptSource.set(nullptr);
    }
}

void
JSCompartment::sweepJitCompartment(FreeOp* fop)
{
    if (jitCompartment_)
        jitCompartment_->sweep(fop, this);
}

void
JSCompartment::sweepRegExps()
{
    /*
     * JIT code increments activeWarmUpCounter for any RegExpShared used by jit
     * code for the lifetime of the JIT script. Thus, we must perform
     * sweeping after clearing jit code.
     */
    regExps.sweep(runtimeFromAnyThread());
}

void
JSCompartment::sweepDebugScopes()
{
    JSRuntime* rt = runtimeFromAnyThread();
    if (debugScopes)
        debugScopes->sweep(rt);
}

void
JSCompartment::sweepWeakMaps()
{
    /* Finalize unreachable (key,value) pairs in all weak maps. */
    WeakMapBase::sweepCompartment(this);
}

void
JSCompartment::sweepNativeIterators()
{
    /* Sweep list of native iterators. */
    NativeIterator* ni = enumerators->next();
    while (ni != enumerators) {
        JSObject* iterObj = ni->iterObj();
        NativeIterator* next = ni->next();
        if (gc::IsObjectAboutToBeFinalizedFromAnyThread(&iterObj))
            ni->unlink();
        ni = next;
    }
}

/*
 * Remove dead wrappers from the table. We must sweep all compartments, since
 * string entries in the crossCompartmentWrappers table are not marked during
 * markCrossCompartmentWrappers.
 */
void
JSCompartment::sweepCrossCompartmentWrappers()
{
    /* Remove dead wrappers from the table. */
    for (WrapperMap::Enum e(crossCompartmentWrappers); !e.empty(); e.popFront()) {
        CrossCompartmentKey key = e.front().key();
        bool keyDying = IsCellAboutToBeFinalizedFromAnyThread(&key.wrapped);
        bool valDying = IsValueAboutToBeFinalizedFromAnyThread(e.front().value().unsafeGet());
        bool dbgDying = key.debugger && IsObjectAboutToBeFinalizedFromAnyThread(&key.debugger);
        if (keyDying || valDying || dbgDying) {
            MOZ_ASSERT(key.kind != CrossCompartmentKey::StringWrapper);
            e.removeFront();
        } else if (key.wrapped != e.front().key().wrapped ||
                   key.debugger != e.front().key().debugger)
        {
            e.rekeyFront(key);
        }
    }
}

void JSCompartment::fixupAfterMovingGC()
{
    fixupGlobal();
    fixupInitialShapeTable();
    fixupBaseShapeTable();
    objectGroups.fixupTablesAfterMovingGC();
}

void
JSCompartment::fixupGlobal()
{
    GlobalObject* global = *global_.unsafeGet();
    if (global)
        global_.set(MaybeForwarded(global));
}

void
JSCompartment::purge()
{
    dtoaCache.purge();
}

void
JSCompartment::clearTables()
{
    global_.set(nullptr);

    // No scripts should have run in this compartment. This is used when
    // merging a compartment that has been used off thread into another
    // compartment and zone.
    MOZ_ASSERT(crossCompartmentWrappers.empty());
    MOZ_ASSERT(!jitCompartment_);
    MOZ_ASSERT(!debugScopes);
    MOZ_ASSERT(!gcWeakMapList);
    MOZ_ASSERT(enumerators->next() == enumerators);
    MOZ_ASSERT(regExps.empty());

    objectGroups.clearTables();
    if (baseShapes.initialized())
        baseShapes.clear();
    if (initialShapes.initialized())
        initialShapes.clear();
    if (savedStacks_.initialized())
        savedStacks_.clear();
}

void
JSCompartment::setObjectMetadataCallback(js::ObjectMetadataCallback callback)
{
    // Clear any jitcode in the runtime, which behaves differently depending on
    // whether there is a creation callback.
    ReleaseAllJITCode(runtime_->defaultFreeOp());

    objectMetadataCallback = callback;
}

static bool
AddInnerLazyFunctionsFromScript(JSScript* script, AutoObjectVector& lazyFunctions)
{
    if (!script->hasObjects())
        return true;
    ObjectArray* objects = script->objects();
    for (size_t i = script->innerObjectsStart(); i < objects->length; i++) {
        JSObject* obj = objects->vector[i];
        if (obj->is<JSFunction>() && obj->as<JSFunction>().isInterpretedLazy()) {
            if (!lazyFunctions.append(obj))
                return false;
        }
    }
    return true;
}

static bool
CreateLazyScriptsForCompartment(JSContext* cx)
{
    AutoObjectVector lazyFunctions(cx);

    // Find all live root lazy functions in the compartment: those which
    // have not been compiled, which have a source object, indicating that
    // they have a parent, and which do not have an uncompiled enclosing
    // script. The last condition is so that we don't compile lazy scripts
    // whose enclosing scripts failed to compile, indicating that the lazy
    // script did not escape the script.
    //
    // Note that while we ideally iterate over LazyScripts, LazyScripts do not
    // currently stand in 1-1 relation with JSScripts; JSFunctions with the
    // same LazyScript may create different JSScripts due to relazification of
    // clones. See bug 1105306.
    for (gc::ZoneCellIter i(cx->zone(), JSFunction::FinalizeKind); !i.done(); i.next()) {
        JSObject* obj = i.get<JSObject>();
        if (obj->compartment() == cx->compartment() && obj->is<JSFunction>()) {
            JSFunction* fun = &obj->as<JSFunction>();
            if (fun->isInterpretedLazy()) {
                LazyScript* lazy = fun->lazyScriptOrNull();
                if (lazy && lazy->sourceObject() && !lazy->maybeScript() &&
                    !lazy->hasUncompiledEnclosingScript())
                {
                    if (!lazyFunctions.append(fun))
                        return false;
                }
            }
        }
    }

    // Create scripts for each lazy function, updating the list of functions to
    // process with any newly exposed inner functions in created scripts.
    // A function cannot be delazified until its outer script exists.
    for (size_t i = 0; i < lazyFunctions.length(); i++) {
        JSFunction* fun = &lazyFunctions[i]->as<JSFunction>();

        // lazyFunctions may have been populated with multiple functions for
        // a lazy script.
        if (!fun->isInterpretedLazy())
            continue;

        LazyScript* lazy = fun->lazyScript();
        bool lazyScriptHadNoScript = !lazy->maybeScript();

        JSScript* script = fun->getOrCreateScript(cx);
        if (!script)
            return false;
        if (lazyScriptHadNoScript && !AddInnerLazyFunctionsFromScript(script, lazyFunctions))
            return false;
    }

    return true;
}

bool
JSCompartment::ensureDelazifyScriptsForDebugger(JSContext* cx)
{
    MOZ_ASSERT(cx->compartment() == this);
    if (needsDelazificationForDebugger() && !CreateLazyScriptsForCompartment(cx))
        return false;
    debugModeBits &= ~DebuggerNeedsDelazification;
    return true;
}

void
JSCompartment::updateDebuggerObservesFlag(unsigned flag)
{
    MOZ_ASSERT(isDebuggee());
    MOZ_ASSERT(flag == DebuggerObservesAllExecution ||
               flag == DebuggerObservesAsmJS);

    const GlobalObject::DebuggerVector* v = maybeGlobal()->getDebuggers();
    for (Debugger * const* p = v->begin(); p != v->end(); p++) {
        Debugger* dbg = *p;
        if (flag == DebuggerObservesAllExecution
            ? dbg->observesAllExecution()
            : dbg->observesAsmJS())
        {
            debugModeBits |= flag;
            return;
        }
    }

    debugModeBits &= ~flag;
}

void
JSCompartment::unsetIsDebuggee()
{
    if (isDebuggee()) {
        debugModeBits &= ~DebuggerObservesMask;
        DebugScopes::onCompartmentUnsetIsDebuggee(this);
    }
}

void
JSCompartment::clearBreakpointsIn(FreeOp* fop, js::Debugger* dbg, HandleObject handler)
{
    for (gc::ZoneCellIter i(zone(), gc::FINALIZE_SCRIPT); !i.done(); i.next()) {
        JSScript* script = i.get<JSScript>();
        if (script->compartment() == this && script->hasAnyBreakpointsOrStepMode())
            script->clearBreakpointsIn(fop, dbg, handler);
    }
}

void
JSCompartment::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                      size_t* tiAllocationSiteTables,
                                      size_t* tiArrayTypeTables,
                                      size_t* tiObjectTypeTables,
                                      size_t* compartmentObject,
                                      size_t* compartmentTables,
                                      size_t* innerViewsArg,
                                      size_t* lazyArrayBuffersArg,
                                      size_t* crossCompartmentWrappersArg,
                                      size_t* regexpCompartment,
                                      size_t* savedStacksSet)
{
    *compartmentObject += mallocSizeOf(this);
    objectGroups.addSizeOfExcludingThis(mallocSizeOf, tiAllocationSiteTables,
                                        tiArrayTypeTables, tiObjectTypeTables,
                                        compartmentTables);
    *compartmentTables += baseShapes.sizeOfExcludingThis(mallocSizeOf)
                        + initialShapes.sizeOfExcludingThis(mallocSizeOf);
    *innerViewsArg += innerViews.sizeOfExcludingThis(mallocSizeOf);
    if (lazyArrayBuffers)
        *lazyArrayBuffersArg += lazyArrayBuffers->sizeOfIncludingThis(mallocSizeOf);
    *crossCompartmentWrappersArg += crossCompartmentWrappers.sizeOfExcludingThis(mallocSizeOf);
    *regexpCompartment += regExps.sizeOfExcludingThis(mallocSizeOf);
    *savedStacksSet += savedStacks_.sizeOfExcludingThis(mallocSizeOf);
}

void
JSCompartment::reportTelemetry()
{
    // Only report telemetry for web content, not add-ons or chrome JS.
    if (addonId || isSystem)
        return;

    // Hazard analysis can't tell that the telemetry callbacks don't GC.
    JS::AutoSuppressGCAnalysis nogc;

    // Call back into Firefox's Telemetry reporter.
    for (size_t i = 0; i < DeprecatedLanguageExtensionCount; i++) {
        if (sawDeprecatedLanguageExtension[i])
            runtime_->addTelemetry(JS_TELEMETRY_DEPRECATED_LANGUAGE_EXTENSIONS_IN_CONTENT, i);
    }
}

void
JSCompartment::addTelemetry(const char* filename, DeprecatedLanguageExtension e)
{
    // Only report telemetry for web content, not add-ons or chrome JS.
    if (addonId || isSystem || !filename || strncmp(filename, "http", 4) != 0)
        return;

    sawDeprecatedLanguageExtension[e] = true;
}

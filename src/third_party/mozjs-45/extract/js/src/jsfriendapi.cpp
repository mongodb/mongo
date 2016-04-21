/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsfriendapi.h"

#include "mozilla/PodOperations.h"

#include <stdint.h>

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsgc.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jswatchpoint.h"
#include "jsweakmap.h"
#include "jswrapper.h"

#include "builtin/TestingFunctions.h"
#include "js/Proxy.h"
#include "proxy/DeadObjectProxy.h"
#include "vm/ArgumentsObject.h"
#include "vm/Time.h"
#include "vm/WrapperObject.h"

#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "vm/NativeObject-inl.h"
#include "vm/ScopeObject-inl.h"

using namespace js;

using mozilla::Move;
using mozilla::PodArrayZero;
using mozilla::UniquePtr;

// Required by PerThreadDataFriendFields::getMainThread()
JS_STATIC_ASSERT(offsetof(JSRuntime, mainThread) ==
                 PerThreadDataFriendFields::RuntimeMainThreadOffset);

PerThreadDataFriendFields::PerThreadDataFriendFields()
{
    PodArrayZero(nativeStackLimit);
#if JS_STACK_GROWTH_DIRECTION > 0
    for (int i=0; i<StackKindCount; i++)
        nativeStackLimit[i] = UINTPTR_MAX;
#endif
}

JS_FRIEND_API(void)
js::SetSourceHook(JSRuntime* rt, UniquePtr<SourceHook> hook)
{
    rt->sourceHook = Move(hook);
}

JS_FRIEND_API(UniquePtr<SourceHook>)
js::ForgetSourceHook(JSRuntime* rt)
{
    return Move(rt->sourceHook);
}

JS_FRIEND_API(void)
JS_SetGrayGCRootsTracer(JSRuntime* rt, JSTraceDataOp traceOp, void* data)
{
    rt->gc.setGrayRootsTracer(traceOp, data);
}

JS_FRIEND_API(JSObject*)
JS_FindCompilationScope(JSContext* cx, HandleObject objArg)
{
    RootedObject obj(cx, objArg);

    /*
     * We unwrap wrappers here. This is a little weird, but it's what's being
     * asked of us.
     */
    if (obj->is<WrapperObject>())
        obj = UncheckedUnwrap(obj);

    /*
     * Get the Window if `obj` is a WindowProxy so that we compile in the
     * correct (global) scope.
     */
    return ToWindowIfWindowProxy(obj);
}

JS_FRIEND_API(JSFunction*)
JS_GetObjectFunction(JSObject* obj)
{
    if (obj->is<JSFunction>())
        return &obj->as<JSFunction>();
    return nullptr;
}

JS_FRIEND_API(bool)
JS_SplicePrototype(JSContext* cx, HandleObject obj, HandleObject proto)
{
    /*
     * Change the prototype of an object which hasn't been used anywhere
     * and does not share its type with another object. Unlike JS_SetPrototype,
     * does not nuke type information for the object.
     */
    CHECK_REQUEST(cx);

    if (!obj->isSingleton()) {
        /*
         * We can see non-singleton objects when trying to splice prototypes
         * due to mutable __proto__ (ugh).
         */
        return JS_SetPrototype(cx, obj, proto);
    }

    Rooted<TaggedProto> tagged(cx, TaggedProto(proto));
    return obj->splicePrototype(cx, obj->getClass(), tagged);
}

JS_FRIEND_API(JSObject*)
JS_NewObjectWithUniqueType(JSContext* cx, const JSClass* clasp, HandleObject proto)
{
    /*
     * Create our object with a null proto and then splice in the correct proto
     * after we setSingleton, so that we don't pollute the default
     * ObjectGroup attached to our proto with information about our object, since
     * we're not going to be using that ObjectGroup anyway.
     */
    RootedObject obj(cx, NewObjectWithGivenProto(cx, (const js::Class*)clasp, nullptr,
                                                 SingletonObject));
    if (!obj)
        return nullptr;
    if (!JS_SplicePrototype(cx, obj, proto))
        return nullptr;
    return obj;
}

JS_FRIEND_API(JSObject*)
JS_NewObjectWithoutMetadata(JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> proto)
{
    // Use an AutoEnterAnalysis to suppress invocation of the metadata callback.
    AutoEnterAnalysis enter(cx);
    return JS_NewObjectWithGivenProto(cx, clasp, proto);
}

JS_FRIEND_API(JSPrincipals*)
JS_GetCompartmentPrincipals(JSCompartment* compartment)
{
    return compartment->principals();
}

JS_FRIEND_API(void)
JS_SetCompartmentPrincipals(JSCompartment* compartment, JSPrincipals* principals)
{
    // Short circuit if there's no change.
    if (principals == compartment->principals())
        return;

    // Any compartment with the trusted principals -- and there can be
    // multiple -- is a system compartment.
    const JSPrincipals* trusted = compartment->runtimeFromMainThread()->trustedPrincipals();
    bool isSystem = principals && principals == trusted;

    // Clear out the old principals, if any.
    if (compartment->principals()) {
        JS_DropPrincipals(compartment->runtimeFromMainThread(), compartment->principals());
        compartment->setPrincipals(nullptr);
        // We'd like to assert that our new principals is always same-origin
        // with the old one, but JSPrincipals doesn't give us a way to do that.
        // But we can at least assert that we're not switching between system
        // and non-system.
        MOZ_ASSERT(compartment->isSystem() == isSystem);
    }

    // Set up the new principals.
    if (principals) {
        JS_HoldPrincipals(principals);
        compartment->setPrincipals(principals);
    }

    // Update the system flag.
    compartment->setIsSystem(isSystem);
}

JS_FRIEND_API(JSPrincipals*)
JS_GetScriptPrincipals(JSScript* script)
{
    return script->principals();
}

JS_FRIEND_API(bool)
JS_ScriptHasMutedErrors(JSScript* script)
{
    return script->mutedErrors();
}

JS_FRIEND_API(bool)
JS_WrapPropertyDescriptor(JSContext* cx, JS::MutableHandle<js::PropertyDescriptor> desc)
{
    return cx->compartment()->wrap(cx, desc);
}

JS_FRIEND_API(void)
JS_TraceShapeCycleCollectorChildren(JS::CallbackTracer* trc, JS::GCCellPtr shape)
{
    MOZ_ASSERT(shape.is<Shape>());
    TraceCycleCollectorChildren(trc, &shape.as<Shape>());
}

JS_FRIEND_API(void)
JS_TraceObjectGroupCycleCollectorChildren(JS::CallbackTracer* trc, JS::GCCellPtr group)
{
    MOZ_ASSERT(group.is<ObjectGroup>());
    TraceCycleCollectorChildren(trc, &group.as<ObjectGroup>());
}

static bool
DefineHelpProperty(JSContext* cx, HandleObject obj, const char* prop, const char* value)
{
    RootedAtom atom(cx, Atomize(cx, value, strlen(value)));
    if (!atom)
        return false;
    return JS_DefineProperty(cx, obj, prop, atom,
                             JSPROP_READONLY | JSPROP_PERMANENT,
                             JS_STUBGETTER, JS_STUBSETTER);
}

JS_FRIEND_API(bool)
JS_DefineFunctionsWithHelp(JSContext* cx, HandleObject obj, const JSFunctionSpecWithHelp* fs)
{
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(cx->compartment()));

    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    for (; fs->name; fs++) {
        JSAtom* atom = Atomize(cx, fs->name, strlen(fs->name));
        if (!atom)
            return false;

        Rooted<jsid> id(cx, AtomToId(atom));
        RootedFunction fun(cx, DefineFunction(cx, obj, id, fs->call, fs->nargs, fs->flags));
        if (!fun)
            return false;

        if (fs->jitInfo)
            fun->setJitInfo(fs->jitInfo);

        if (fs->usage) {
            if (!DefineHelpProperty(cx, fun, "usage", fs->usage))
                return false;
        }

        if (fs->help) {
            if (!DefineHelpProperty(cx, fun, "help", fs->help))
                return false;
        }
    }

    return true;
}

JS_FRIEND_API(bool)
js::GetBuiltinClass(JSContext* cx, HandleObject obj, ESClassValue* classValue)
{
    if (MOZ_UNLIKELY(obj->is<ProxyObject>()))
        return Proxy::getBuiltinClass(cx, obj, classValue);

    if (obj->is<PlainObject>() || obj->is<UnboxedPlainObject>())
        *classValue = ESClass_Object;
    else if (obj->is<ArrayObject>() || obj->is<UnboxedArrayObject>())
        *classValue = ESClass_Array;
    else if (obj->is<NumberObject>())
        *classValue = ESClass_Number;
    else if (obj->is<StringObject>())
        *classValue = ESClass_String;
    else if (obj->is<BooleanObject>())
        *classValue = ESClass_Boolean;
    else if (obj->is<RegExpObject>())
        *classValue = ESClass_RegExp;
    else if (obj->is<ArrayBufferObject>())
        *classValue = ESClass_ArrayBuffer;
    else if (obj->is<SharedArrayBufferObject>())
        *classValue = ESClass_SharedArrayBuffer;
    else if (obj->is<DateObject>())
        *classValue = ESClass_Date;
    else if (obj->is<SetObject>())
        *classValue = ESClass_Set;
    else if (obj->is<MapObject>())
        *classValue = ESClass_Map;
    else
        *classValue = ESClass_Other;

    return true;
}

JS_FRIEND_API(const char*)
js::ObjectClassName(JSContext* cx, HandleObject obj)
{
    return GetObjectClassName(cx, obj);
}

JS_FRIEND_API(JS::Zone*)
js::GetCompartmentZone(JSCompartment* comp)
{
    return comp->zone();
}

JS_FRIEND_API(bool)
js::IsSystemCompartment(JSCompartment* comp)
{
    return comp->isSystem();
}

JS_FRIEND_API(bool)
js::IsSystemZone(Zone* zone)
{
    return zone->isSystem;
}

JS_FRIEND_API(bool)
js::IsAtomsCompartment(JSCompartment* comp)
{
    return comp->runtimeFromAnyThread()->isAtomsCompartment(comp);
}

JS_FRIEND_API(bool)
js::IsAtomsZone(JS::Zone* zone)
{
    return zone->runtimeFromAnyThread()->isAtomsZone(zone);
}

JS_FRIEND_API(bool)
js::IsFunctionObject(JSObject* obj)
{
    return obj->is<JSFunction>();
}

JS_FRIEND_API(JSObject*)
js::GetGlobalForObjectCrossCompartment(JSObject* obj)
{
    return &obj->global();
}

JS_FRIEND_API(JSObject*)
js::GetPrototypeNoProxy(JSObject* obj)
{
    MOZ_ASSERT(!obj->is<js::ProxyObject>());
    MOZ_ASSERT(!obj->getTaggedProto().isLazy());
    return obj->getTaggedProto().toObjectOrNull();
}

JS_FRIEND_API(void)
js::AssertSameCompartment(JSContext* cx, JSObject* obj)
{
    assertSameCompartment(cx, obj);
}

#ifdef DEBUG
JS_FRIEND_API(void)
js::AssertSameCompartment(JSObject* objA, JSObject* objB)
{
    MOZ_ASSERT(objA->compartment() == objB->compartment());
}
#endif

JS_FRIEND_API(void)
js::NotifyAnimationActivity(JSObject* obj)
{
    int64_t timeNow = PRMJ_Now();
    obj->compartment()->lastAnimationTime = timeNow;
    obj->runtimeFromMainThread()->lastAnimationTime = timeNow;
}

JS_FRIEND_API(uint32_t)
js::GetObjectSlotSpan(JSObject* obj)
{
    return obj->as<NativeObject>().slotSpan();
}

JS_FRIEND_API(bool)
js::IsObjectInContextCompartment(JSObject* obj, const JSContext* cx)
{
    return obj->compartment() == cx->compartment();
}

JS_FRIEND_API(bool)
js::RunningWithTrustedPrincipals(JSContext* cx)
{
    return cx->runningWithTrustedPrincipals();
}

JS_FRIEND_API(JSFunction*)
js::GetOutermostEnclosingFunctionOfScriptedCaller(JSContext* cx)
{
    ScriptFrameIter iter(cx);
    if (iter.done())
        return nullptr;

    if (!iter.isFunctionFrame())
        return nullptr;

    RootedFunction curr(cx, iter.callee(cx));
    for (StaticScopeIter<NoGC> i(curr); !i.done(); i++) {
        if (i.type() == StaticScopeIter<NoGC>::Function)
            curr = &i.fun();
    }
    return curr;
}

JS_FRIEND_API(JSFunction*)
js::DefineFunctionWithReserved(JSContext* cx, JSObject* objArg, const char* name, JSNative call,
                               unsigned nargs, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(cx->compartment()));
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    JSAtom* atom = Atomize(cx, name, strlen(name));
    if (!atom)
        return nullptr;
    Rooted<jsid> id(cx, AtomToId(atom));
    return DefineFunction(cx, obj, id, call, nargs, attrs, gc::AllocKind::FUNCTION_EXTENDED);
}

JS_FRIEND_API(JSFunction*)
js::NewFunctionWithReserved(JSContext* cx, JSNative native, unsigned nargs, unsigned flags,
                            const char* name)
{
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(cx->compartment()));

    CHECK_REQUEST(cx);

    RootedAtom atom(cx);
    if (name) {
        atom = Atomize(cx, name, strlen(name));
        if (!atom)
            return nullptr;
    }

    return (flags & JSFUN_CONSTRUCTOR) ?
        NewNativeConstructor(cx, native, nargs, atom, gc::AllocKind::FUNCTION_EXTENDED) :
        NewNativeFunction(cx, native, nargs, atom, gc::AllocKind::FUNCTION_EXTENDED);
}

JS_FRIEND_API(JSFunction*)
js::NewFunctionByIdWithReserved(JSContext* cx, JSNative native, unsigned nargs, unsigned flags,
                                jsid id)
{
    MOZ_ASSERT(JSID_IS_STRING(id));
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(cx->compartment()));
    CHECK_REQUEST(cx);

    RootedAtom atom(cx, JSID_TO_ATOM(id));
    return (flags & JSFUN_CONSTRUCTOR) ?
        NewNativeConstructor(cx, native, nargs, atom, gc::AllocKind::FUNCTION_EXTENDED) :
        NewNativeFunction(cx, native, nargs, atom, gc::AllocKind::FUNCTION_EXTENDED);
}

JS_FRIEND_API(const Value&)
js::GetFunctionNativeReserved(JSObject* fun, size_t which)
{
    MOZ_ASSERT(fun->as<JSFunction>().isNative());
    return fun->as<JSFunction>().getExtendedSlot(which);
}

JS_FRIEND_API(void)
js::SetFunctionNativeReserved(JSObject* fun, size_t which, const Value& val)
{
    MOZ_ASSERT(fun->as<JSFunction>().isNative());
    MOZ_ASSERT_IF(val.isObject(), val.toObject().compartment() == fun->compartment());
    fun->as<JSFunction>().setExtendedSlot(which, val);
}

JS_FRIEND_API(bool)
js::FunctionHasNativeReserved(JSObject* fun)
{
    MOZ_ASSERT(fun->as<JSFunction>().isNative());
    return fun->as<JSFunction>().isExtended();
}

JS_FRIEND_API(bool)
js::GetObjectProto(JSContext* cx, JS::Handle<JSObject*> obj, JS::MutableHandle<JSObject*> proto)
{
    if (IsProxy(obj))
        return JS_GetPrototype(cx, obj, proto);

    proto.set(reinterpret_cast<const shadow::Object*>(obj.get())->group->proto);
    return true;
}

JS_FRIEND_API(bool)
js::GetOriginalEval(JSContext* cx, HandleObject scope, MutableHandleObject eval)
{
    assertSameCompartment(cx, scope);
    Rooted<GlobalObject*> global(cx, &scope->global());
    return GlobalObject::getOrCreateEval(cx, global, eval);
}

JS_FRIEND_API(void)
js::SetReservedOrProxyPrivateSlotWithBarrier(JSObject* obj, size_t slot, const js::Value& value)
{
    if (IsProxy(obj)) {
        MOZ_ASSERT(slot == 0);
        obj->as<ProxyObject>().setSameCompartmentPrivate(value);
    } else {
        obj->as<NativeObject>().setSlot(slot, value);
    }
}

void
js::SetPreserveWrapperCallback(JSRuntime* rt, PreserveWrapperCallback callback)
{
    rt->preserveWrapperCallback = callback;
}

/*
 * The below code is for temporary telemetry use. It can be removed when
 * sufficient data has been harvested.
 */

namespace js {
// Defined in vm/GlobalObject.cpp.
extern size_t sSetProtoCalled;
} // namespace js

JS_FRIEND_API(size_t)
JS_SetProtoCalled(JSContext*)
{
    return sSetProtoCalled;
}

// Defined in jsiter.cpp.
extern size_t sCustomIteratorCount;

JS_FRIEND_API(size_t)
JS_GetCustomIteratorCount(JSContext* cx)
{
    return sCustomIteratorCount;
}

JS_FRIEND_API(unsigned)
JS_PCToLineNumber(JSScript* script, jsbytecode* pc, unsigned* columnp)
{
    return PCToLineNumber(script, pc, columnp);
}

JS_FRIEND_API(bool)
JS_IsDeadWrapper(JSObject* obj)
{
    return IsDeadProxyObject(obj);
}

void
js::TraceWeakMaps(WeakMapTracer* trc)
{
    WeakMapBase::traceAllMappings(trc);
    WatchpointMap::traceAll(trc);
}

extern JS_FRIEND_API(bool)
js::AreGCGrayBitsValid(JSRuntime* rt)
{
    return rt->gc.areGrayBitsValid();
}

JS_FRIEND_API(bool)
js::ZoneGlobalsAreAllGray(JS::Zone* zone)
{
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
        JSObject* obj = comp->maybeGlobal();
        if (!obj || !JS::ObjectIsMarkedGray(obj))
            return false;
    }
    return true;
}

JS_FRIEND_API(JS::TraceKind)
js::GCThingTraceKind(void* thing)
{
    MOZ_ASSERT(thing);
    return static_cast<js::gc::Cell*>(thing)->getTraceKind();
}

JS_FRIEND_API(void)
js::VisitGrayWrapperTargets(Zone* zone, GCThingCallback callback, void* closure)
{
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
        for (JSCompartment::WrapperEnum e(comp); !e.empty(); e.popFront()) {
            gc::Cell* thing = e.front().key().wrapped;
            if (thing->isTenured() && thing->asTenured().isMarked(gc::GRAY))
                callback(closure, JS::GCCellPtr(thing, thing->asTenured().getTraceKind()));
        }
    }
}

JS_FRIEND_API(JSObject*)
js::GetWeakmapKeyDelegate(JSObject* key)
{
    if (JSWeakmapKeyDelegateOp op = key->getClass()->ext.weakmapKeyDelegateOp)
        return op(key);
    return nullptr;
}

JS_FRIEND_API(JSLinearString*)
js::StringToLinearStringSlow(JSContext* cx, JSString* str)
{
    return str->ensureLinear(cx);
}

JS_FRIEND_API(void)
JS_SetAccumulateTelemetryCallback(JSRuntime* rt, JSAccumulateTelemetryDataCallback callback)
{
    rt->setTelemetryCallback(rt, callback);
}

JS_FRIEND_API(JSObject*)
JS_CloneObject(JSContext* cx, HandleObject obj, HandleObject protoArg)
{
    Rooted<TaggedProto> proto(cx, TaggedProto(protoArg.get()));
    return CloneObject(cx, obj, proto);
}

#ifdef DEBUG
JS_FRIEND_API(void)
js::DumpString(JSString* str)
{
    str->dump();
}

JS_FRIEND_API(void)
js::DumpAtom(JSAtom* atom)
{
    atom->dump();
}

JS_FRIEND_API(void)
js::DumpChars(const char16_t* s, size_t n)
{
    fprintf(stderr, "char16_t * (%p) = ", (void*) s);
    JSString::dumpChars(s, n);
    fputc('\n', stderr);
}

JS_FRIEND_API(void)
js::DumpObject(JSObject* obj)
{
    if (!obj) {
        fprintf(stderr, "NULL\n");
        return;
    }
    obj->dump();
}

#endif

static const char*
FormatValue(JSContext* cx, const Value& vArg, JSAutoByteString& bytes)
{
    RootedValue v(cx, vArg);

    if (v.isMagic(JS_OPTIMIZED_OUT))
        return "[unavailable]";

    /*
     * We could use Maybe<AutoCompartment> here, but G++ can't quite follow
     * that, and warns about uninitialized members being used in the
     * destructor.
     */
    RootedString str(cx);
    if (v.isObject()) {
        AutoCompartment ac(cx, &v.toObject());
        str = ToString<CanGC>(cx, v);
    } else {
        str = ToString<CanGC>(cx, v);
    }

    if (!str)
        return nullptr;
    const char* buf = bytes.encodeLatin1(cx, str);
    if (!buf)
        return nullptr;
    const char* found = strstr(buf, "function ");
    if (found && (found - buf <= 2))
        return "[function]";
    return buf;
}

// Wrapper for JS_sprintf_append() that reports allocation failure to the
// context.
template <typename... Args>
static char*
sprintf_append(JSContext* cx, char* buf, Args&&... args)
{
    char* result = JS_sprintf_append(buf, mozilla::Forward<Args>(args)...);
    if (!result) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    return result;
}

static char*
FormatFrame(JSContext* cx, const ScriptFrameIter& iter, char* buf, int num,
            bool showArgs, bool showLocals, bool showThisProps)
{
    MOZ_ASSERT(!cx->isExceptionPending());
    RootedScript script(cx, iter.script());
    jsbytecode* pc = iter.pc();

    RootedObject scopeChain(cx, iter.scopeChain(cx));
    JSAutoCompartment ac(cx, scopeChain);

    const char* filename = script->filename();
    unsigned lineno = PCToLineNumber(script, pc);
    RootedFunction fun(cx, iter.maybeCallee(cx));
    RootedString funname(cx);
    if (fun)
        funname = fun->displayAtom();

    RootedValue thisVal(cx);
    if (iter.hasUsableAbstractFramePtr() &&
        iter.isNonEvalFunctionFrame() &&
        fun && !fun->isArrow() && !fun->isDerivedClassConstructor())
    {
        if (!GetFunctionThis(cx, iter.abstractFramePtr(), &thisVal))
            return nullptr;
    }

    // print the frame number and function name
    if (funname) {
        JSAutoByteString funbytes;
        char* str = funbytes.encodeLatin1(cx, funname);
        if (!str)
            return nullptr;
        buf = sprintf_append(cx, buf, "%d %s(", num, str);
    } else if (fun) {
        buf = sprintf_append(cx, buf, "%d anonymous(", num);
    } else {
        buf = sprintf_append(cx, buf, "%d <TOP LEVEL>", num);
    }
    if (!buf)
        return nullptr;

    if (showArgs && iter.hasArgs()) {
        BindingIter bi(script);
        bool first = true;
        for (unsigned i = 0; i < iter.numActualArgs(); i++) {
            RootedValue arg(cx);
            if (i < iter.numFormalArgs() && script->formalIsAliased(i)) {
                for (AliasedFormalIter fi(script); ; fi++) {
                    if (fi.frameIndex() == i) {
                        arg = iter.callObj(cx).aliasedVar(fi);
                        break;
                    }
                }
            } else if (script->argsObjAliasesFormals() && iter.hasArgsObj()) {
                arg = iter.argsObj().arg(i);
            } else {
                if (iter.hasUsableAbstractFramePtr())
                    arg = iter.unaliasedActual(i, DONT_CHECK_ALIASING);
                else
                    arg = MagicValue(JS_OPTIMIZED_OUT);
            }

            JSAutoByteString valueBytes;
            const char* value = FormatValue(cx, arg, valueBytes);
            if (!value) {
                if (cx->isThrowingOutOfMemory())
                    return nullptr;
                cx->clearPendingException();
            }

            JSAutoByteString nameBytes;
            const char* name = nullptr;

            if (i < iter.numFormalArgs()) {
                MOZ_ASSERT(i == bi.argIndex());
                name = nameBytes.encodeLatin1(cx, bi->name());
                if (!name)
                    return nullptr;
                bi++;
            }

            if (value) {
                buf = sprintf_append(cx, buf, "%s%s%s%s%s%s",
                                     !first ? ", " : "",
                                     name ? name :"",
                                     name ? " = " : "",
                                     arg.isString() ? "\"" : "",
                                     value,
                                     arg.isString() ? "\"" : "");
                if (!buf)
                    return nullptr;

                first = false;
            } else {
                buf = sprintf_append(cx, buf,
                                     "    <Failed to get argument while inspecting stack frame>\n");
                if (!buf)
                    return nullptr;

            }
        }
    }

    // print filename and line number
    buf = sprintf_append(cx, buf, "%s [\"%s\":%d]\n",
                         fun ? ")" : "",
                         filename ? filename : "<unknown>",
                         lineno);
    if (!buf)
        return nullptr;


    // Note: Right now we don't dump the local variables anymore, because
    // that is hard to support across all the JITs etc.

    // print the value of 'this'
    if (showLocals) {
        if (!thisVal.isUndefined()) {
            JSAutoByteString thisValBytes;
            RootedString thisValStr(cx, ToString<CanGC>(cx, thisVal));
            if (!thisValStr) {
                if (cx->isThrowingOutOfMemory())
                    return nullptr;
                cx->clearPendingException();
            }
            if (thisValStr) {
                const char* str = thisValBytes.encodeLatin1(cx, thisValStr);
                if (!str)
                    return nullptr;
                buf = sprintf_append(cx, buf, "    this = %s\n", str);
            } else {
                buf = sprintf_append(cx, buf, "    <failed to get 'this' value>\n");
            }
            if (!buf)
                return nullptr;
        }
    }

    if (showThisProps && thisVal.isObject()) {
        RootedObject obj(cx, &thisVal.toObject());

        AutoIdVector keys(cx);
        if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, &keys)) {
            if (cx->isThrowingOutOfMemory())
                return nullptr;
            cx->clearPendingException();
        }

        RootedId id(cx);
        for (size_t i = 0; i < keys.length(); i++) {
            RootedId id(cx, keys[i]);
            RootedValue key(cx, IdToValue(id));
            RootedValue v(cx);

            if (!GetProperty(cx, obj, obj, id, &v)) {
                if (cx->isThrowingOutOfMemory())
                    return nullptr;
                cx->clearPendingException();
                buf = sprintf_append(cx, buf,
                                     "    <Failed to fetch property while inspecting stack frame>\n");
                if (!buf)
                    return nullptr;
                continue;
            }

            JSAutoByteString nameBytes;
            const char* name = FormatValue(cx, key, nameBytes);
            if (!name) {
                if (cx->isThrowingOutOfMemory())
                    return nullptr;
                cx->clearPendingException();
            }

            JSAutoByteString valueBytes;
            const char* value = FormatValue(cx, v, valueBytes);
            if (!value) {
                if (cx->isThrowingOutOfMemory())
                    return nullptr;
                cx->clearPendingException();
            }

            if (name && value) {
                buf = sprintf_append(cx, buf, "    this.%s = %s%s%s\n",
                                     name,
                                     v.isString() ? "\"" : "",
                                     value,
                                     v.isString() ? "\"" : "");
            } else {
                buf = sprintf_append(cx, buf,
                                     "    <Failed to format values while inspecting stack frame>\n");
            }
            if (!buf)
                return nullptr;
        }
    }

    MOZ_ASSERT(!cx->isExceptionPending());
    return buf;
}

JS_FRIEND_API(char*)
JS::FormatStackDump(JSContext* cx, char* buf, bool showArgs, bool showLocals, bool showThisProps)
{
    int num = 0;

    for (AllFramesIter i(cx); !i.done(); ++i) {
        buf = FormatFrame(cx, i, buf, num, showArgs, showLocals, showThisProps);
        if (!buf)
            return nullptr;
        num++;
    }

    if (!num)
        buf = JS_sprintf_append(buf, "JavaScript stack is empty\n");

    return buf;
}

struct DumpHeapTracer : public JS::CallbackTracer, public WeakMapTracer
{
    const char* prefix;
    FILE* output;

    DumpHeapTracer(FILE* fp, JSRuntime* rt)
      : JS::CallbackTracer(rt, DoNotTraceWeakMaps),
        js::WeakMapTracer(rt), prefix(""), output(fp)
    {}

  private:
    void trace(JSObject* map, JS::GCCellPtr key, JS::GCCellPtr value) override {
        JSObject* kdelegate = nullptr;
        if (key.is<JSObject>())
            kdelegate = js::GetWeakmapKeyDelegate(&key.as<JSObject>());

        fprintf(output, "WeakMapEntry map=%p key=%p keyDelegate=%p value=%p\n",
                map, key.asCell(), kdelegate, value.asCell());
    }

    void onChild(const JS::GCCellPtr& thing) override;
};

static char
MarkDescriptor(void* thing)
{
    gc::TenuredCell* cell = gc::TenuredCell::fromPointer(thing);
    if (cell->isMarked(gc::BLACK))
        return cell->isMarked(gc::GRAY) ? 'G' : 'B';
    else
        return cell->isMarked(gc::GRAY) ? 'X' : 'W';
}

static void
DumpHeapVisitZone(JSRuntime* rt, void* data, Zone* zone)
{
    DumpHeapTracer* dtrc = static_cast<DumpHeapTracer*>(data);
    fprintf(dtrc->output, "# zone %p\n", (void*)zone);
}

static void
DumpHeapVisitCompartment(JSRuntime* rt, void* data, JSCompartment* comp)
{
    char name[1024];
    if (rt->compartmentNameCallback)
        (*rt->compartmentNameCallback)(rt, comp, name, sizeof(name));
    else
        strcpy(name, "<unknown>");

    DumpHeapTracer* dtrc = static_cast<DumpHeapTracer*>(data);
    fprintf(dtrc->output, "# compartment %s [in zone %p]\n", name, (void*)comp->zone());
}

static void
DumpHeapVisitArena(JSRuntime* rt, void* data, gc::Arena* arena,
                   JS::TraceKind traceKind, size_t thingSize)
{
    DumpHeapTracer* dtrc = static_cast<DumpHeapTracer*>(data);
    fprintf(dtrc->output, "# arena allockind=%u size=%u\n",
            unsigned(arena->aheader.getAllocKind()), unsigned(thingSize));
}

static void
DumpHeapVisitCell(JSRuntime* rt, void* data, void* thing,
                  JS::TraceKind traceKind, size_t thingSize)
{
    DumpHeapTracer* dtrc = static_cast<DumpHeapTracer*>(data);
    char cellDesc[1024 * 32];
    JS_GetTraceThingInfo(cellDesc, sizeof(cellDesc), dtrc, thing, traceKind, true);
    fprintf(dtrc->output, "%p %c %s\n", thing, MarkDescriptor(thing), cellDesc);
    js::TraceChildren(dtrc, thing, traceKind);
}

void
DumpHeapTracer::onChild(const JS::GCCellPtr& thing)
{
    if (gc::IsInsideNursery(thing.asCell()))
        return;

    char buffer[1024];
    getTracingEdgeName(buffer, sizeof(buffer));
    fprintf(output, "%s%p %c %s\n", prefix, thing.asCell(), MarkDescriptor(thing.asCell()), buffer);
}

void
js::DumpHeap(JSRuntime* rt, FILE* fp, js::DumpHeapNurseryBehaviour nurseryBehaviour)
{
    if (nurseryBehaviour == js::CollectNurseryBeforeDump)
        rt->gc.evictNursery(JS::gcreason::API);

    DumpHeapTracer dtrc(fp, rt);
    fprintf(dtrc.output, "# Roots.\n");
    TraceRuntime(&dtrc);

    fprintf(dtrc.output, "# Weak maps.\n");
    WeakMapBase::traceAllMappings(&dtrc);

    fprintf(dtrc.output, "==========\n");

    dtrc.prefix = "> ";
    IterateZonesCompartmentsArenasCells(rt, &dtrc,
                                        DumpHeapVisitZone,
                                        DumpHeapVisitCompartment,
                                        DumpHeapVisitArena,
                                        DumpHeapVisitCell);

    fflush(dtrc.output);
}

JS_FRIEND_API(bool)
js::ContextHasOutstandingRequests(const JSContext* cx)
{
    return cx->outstandingRequests > 0;
}

JS_FRIEND_API(void)
js::SetActivityCallback(JSRuntime* rt, ActivityCallback cb, void* arg)
{
    rt->activityCallback = cb;
    rt->activityCallbackArg = arg;
}

JS_FRIEND_API(void)
JS::NotifyDidPaint(JSRuntime* rt)
{
    rt->gc.notifyDidPaint();
}

JS_FRIEND_API(void)
JS::PokeGC(JSRuntime* rt)
{
    rt->gc.poke();
}

JS_FRIEND_API(JSCompartment*)
js::GetAnyCompartmentInZone(JS::Zone* zone)
{
    CompartmentsInZoneIter comp(zone);
    MOZ_ASSERT(!comp.done());
    return comp.get();
}

void
JS::ObjectPtr::updateWeakPointerAfterGC()
{
    if (js::gc::IsAboutToBeFinalizedUnbarriered(value.unsafeGet()))
        value = nullptr;
}

void
JS::ObjectPtr::trace(JSTracer* trc, const char* name)
{
    JS_CallObjectTracer(trc, &value, name);
}

JS_FRIEND_API(JSObject*)
js::GetTestingFunctions(JSContext* cx)
{
    RootedObject obj(cx, JS_NewPlainObject(cx));
    if (!obj)
        return nullptr;

    if (!DefineTestingFunctions(cx, obj, false, false))
        return nullptr;

    return obj;
}

#ifdef DEBUG
JS_FRIEND_API(unsigned)
js::GetEnterCompartmentDepth(JSContext* cx)
{
  return cx->getEnterCompartmentDepth();
}
#endif

JS_FRIEND_API(void)
js::SetDOMCallbacks(JSRuntime* rt, const DOMCallbacks* callbacks)
{
    rt->DOMcallbacks = callbacks;
}

JS_FRIEND_API(const DOMCallbacks*)
js::GetDOMCallbacks(JSRuntime* rt)
{
    return rt->DOMcallbacks;
}

static const void* gDOMProxyHandlerFamily = nullptr;
static uint32_t gDOMProxyExpandoSlot = 0;
static DOMProxyShadowsCheck gDOMProxyShadowsCheck;

JS_FRIEND_API(void)
js::SetDOMProxyInformation(const void* domProxyHandlerFamily, uint32_t domProxyExpandoSlot,
                           DOMProxyShadowsCheck domProxyShadowsCheck)
{
    gDOMProxyHandlerFamily = domProxyHandlerFamily;
    gDOMProxyExpandoSlot = domProxyExpandoSlot;
    gDOMProxyShadowsCheck = domProxyShadowsCheck;
}

const void*
js::GetDOMProxyHandlerFamily()
{
    return gDOMProxyHandlerFamily;
}

uint32_t
js::GetDOMProxyExpandoSlot()
{
    return gDOMProxyExpandoSlot;
}

DOMProxyShadowsCheck
js::GetDOMProxyShadowsCheck()
{
    return gDOMProxyShadowsCheck;
}

bool
js::detail::IdMatchesAtom(jsid id, JSAtom* atom)
{
    return id == INTERNED_STRING_TO_JSID(nullptr, atom);
}

JS_FRIEND_API(void)
js::PrepareScriptEnvironmentAndInvoke(JSContext* cx, HandleObject scope, ScriptEnvironmentPreparer::Closure& closure)
{
    MOZ_ASSERT(!cx->isExceptionPending());

    if (cx->runtime()->scriptEnvironmentPreparer) {
        cx->runtime()->scriptEnvironmentPreparer->invoke(scope, closure);
        return;
    }

    JSAutoCompartment ac(cx, scope);
    bool ok = closure(cx);

    MOZ_ASSERT_IF(ok, !cx->isExceptionPending());

    // NB: This does not affect Gecko, which has a prepareScriptEnvironment
    // callback.
    if (!ok) {
        JS_ReportPendingException(cx);
    }

    MOZ_ASSERT(!cx->isExceptionPending());
}

JS_FRIEND_API(void)
js::SetScriptEnvironmentPreparer(JSRuntime* rt, ScriptEnvironmentPreparer* preparer)
{
    rt->scriptEnvironmentPreparer = preparer;
}

#ifdef DEBUG
JS_FRIEND_API(void)
js::Debug_SetActiveJSContext(JSRuntime* rt, JSContext* cx)
{
    rt->activeContext = cx;
}
#endif

JS_FRIEND_API(void)
js::SetCTypesActivityCallback(JSRuntime* rt, CTypesActivityCallback cb)
{
    rt->ctypesActivityCallback = cb;
}

js::AutoCTypesActivityCallback::AutoCTypesActivityCallback(JSContext* cx,
                                                           js::CTypesActivityType beginType,
                                                           js::CTypesActivityType endType
                                                           MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
  : cx(cx), callback(cx->runtime()->ctypesActivityCallback), endType(endType)
{
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;

    if (callback)
        callback(cx, beginType);
}

JS_FRIEND_API(void)
js::SetObjectMetadataCallback(JSContext* cx, ObjectMetadataCallback callback)
{
    cx->compartment()->setObjectMetadataCallback(callback);
}

JS_FRIEND_API(JSObject*)
js::GetObjectMetadata(JSObject* obj)
{
    ObjectWeakMap* map = obj->compartment()->objectMetadataTable;
    if (map)
        return map->lookup(obj);
    return nullptr;
}

JS_FRIEND_API(bool)
js::ReportIsNotFunction(JSContext* cx, HandleValue v)
{
    return ReportIsNotFunction(cx, v, -1);
}

JS_FRIEND_API(void)
js::ReportErrorWithId(JSContext* cx, const char* msg, HandleId id)
{
    RootedValue idv(cx);
    if (!JS_IdToValue(cx, id, &idv))
        return;
    JSString* idstr = JS::ToString(cx, idv);
    if (!idstr)
        return;
    JSAutoByteString bytes(cx, idstr);
    if (!bytes)
        return;
    JS_ReportError(cx, msg, bytes.ptr());
}

#ifdef DEBUG
bool
js::HasObjectMovedOp(JSObject* obj) {
    return !!GetObjectClass(obj)->ext.objectMovedOp;
}
#endif

JS_FRIEND_API(void)
JS_StoreObjectPostBarrierCallback(JSContext* cx,
                                  void (*callback)(JSTracer* trc, JSObject* key, void* data),
                                  JSObject* key, void* data)
{
    JSRuntime* rt = cx->runtime();
    if (IsInsideNursery(key))
        rt->gc.storeBuffer.putCallback(callback, key, data);
}

extern JS_FRIEND_API(void)
JS_StoreStringPostBarrierCallback(JSContext* cx,
                                  void (*callback)(JSTracer* trc, JSString* key, void* data),
                                  JSString* key, void* data)
{
    JSRuntime* rt = cx->runtime();
    if (IsInsideNursery(key))
        rt->gc.storeBuffer.putCallback(callback, key, data);
}

extern JS_FRIEND_API(void)
JS_ClearAllPostBarrierCallbacks(JSRuntime* rt)
{
    rt->gc.clearPostBarrierCallbacks();
}

JS_FRIEND_API(bool)
js::ForwardToNative(JSContext* cx, JSNative native, const CallArgs& args)
{
    return native(cx, args.length(), args.base());
}

JS_FRIEND_API(JSObject*)
js::ConvertArgsToArray(JSContext* cx, const CallArgs& args)
{
    RootedObject argsArray(cx, NewDenseCopiedArray(cx, args.length(), args.array()));
    return argsArray;
}

JS_FRIEND_API(JSAtom*)
js::GetPropertyNameFromPC(JSScript* script, jsbytecode* pc)
{
    if (!IsGetPropPC(pc) && !IsSetPropPC(pc))
        return nullptr;
    return script->getName(pc);
}

JS_FRIEND_API(void)
js::SetWindowProxyClass(JSRuntime* rt, const js::Class* clasp)
{
    MOZ_ASSERT(!rt->maybeWindowProxyClass());
    rt->setWindowProxyClass(clasp);
}

JS_FRIEND_API(void)
js::SetWindowProxy(JSContext* cx, HandleObject global, HandleObject windowProxy)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    assertSameCompartment(cx, global, windowProxy);

    MOZ_ASSERT(IsWindowProxy(windowProxy));
    global->as<GlobalObject>().setWindowProxy(windowProxy);
}

JS_FRIEND_API(JSObject*)
js::ToWindowIfWindowProxy(JSObject* obj)
{
    if (IsWindowProxy(obj))
        return &obj->global();
    return obj;
}

JS_FRIEND_API(JSObject*)
js::ToWindowProxyIfWindow(JSObject* obj)
{
    if (IsWindow(obj))
        return obj->as<GlobalObject>().windowProxy();
    return obj;
}

JS_FRIEND_API(bool)
js::IsWindowProxy(JSObject* obj)
{
    // Note: simply checking `obj == obj->global().windowProxy()` is not
    // sufficient: we may have transplanted the window proxy with a CCW.
    // Check the Class to ensure we really have a window proxy.
    return obj->getClass() == obj->runtimeFromAnyThread()->maybeWindowProxyClass();
}

JS_FRIEND_API(bool)
js::detail::IsWindowSlow(JSObject* obj)
{
    return obj->as<GlobalObject>().maybeWindowProxy();
}

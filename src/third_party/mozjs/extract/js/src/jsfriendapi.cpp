/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsfriendapi.h"

#include "mozilla/PodOperations.h"

#include <stdint.h>

#include "builtin/Promise.h"
#include "builtin/TestingFunctions.h"
#include "gc/GCInternals.h"
#include "gc/PublicIterators.h"
#include "gc/WeakMap.h"
#include "js/Printf.h"
#include "js/Proxy.h"
#include "js/Wrapper.h"
#include "proxy/DeadObjectProxy.h"
#include "vm/ArgumentsObject.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/Time.h"
#include "vm/WrapperObject.h"

#include "gc/Nursery-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::Move;
using mozilla::PodArrayZero;

JS::RootingContext::RootingContext()
  : autoGCRooters_(nullptr), compartment_(nullptr), zone_(nullptr)
{
    for (auto& stackRootPtr : stackRoots_)
        stackRootPtr = nullptr;

    PodArrayZero(nativeStackLimit);
#if JS_STACK_GROWTH_DIRECTION > 0
    for (int i=0; i<StackKindCount; i++)
        nativeStackLimit[i] = UINTPTR_MAX;
#endif
}

JS_FRIEND_API(void)
js::SetSourceHook(JSContext* cx, mozilla::UniquePtr<SourceHook> hook)
{
    cx->runtime()->sourceHook.ref() = Move(hook);
}

JS_FRIEND_API(mozilla::UniquePtr<SourceHook>)
js::ForgetSourceHook(JSContext* cx)
{
    return Move(cx->runtime()->sourceHook.ref());
}

JS_FRIEND_API(void)
JS_SetGrayGCRootsTracer(JSContext* cx, JSTraceDataOp traceOp, void* data)
{
    cx->runtime()->gc.setGrayRootsTracer(traceOp, data);
}

JS_FRIEND_API(JSObject*)
JS_FindCompilationScope(JSContext* cx, HandleObject objArg)
{
    assertSameCompartment(cx, objArg);

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
    assertSameCompartment(cx, obj, proto);

    if (!obj->isSingleton()) {
        /*
         * We can see non-singleton objects when trying to splice prototypes
         * due to mutable __proto__ (ugh).
         */
        return JS_SetPrototype(cx, obj, proto);
    }

    Rooted<TaggedProto> tagged(cx, TaggedProto(proto));
    return JSObject::splicePrototype(cx, obj, obj->getClass(), tagged);
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
    assertSameCompartment(cx, proto);
    AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);
    return JS_NewObjectWithGivenProto(cx, clasp, proto);
}

JS_FRIEND_API(bool)
JS_GetIsSecureContext(JSCompartment* compartment)
{
    return compartment->creationOptions().secureContext();
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
    const JSPrincipals* trusted = compartment->runtimeFromActiveCooperatingThread()->trustedPrincipals();
    bool isSystem = principals && principals == trusted;

    // Clear out the old principals, if any.
    if (compartment->principals()) {
        JS_DropPrincipals(TlsContext.get(), compartment->principals());
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

JS_FRIEND_API(JSCompartment*)
js::GetScriptCompartment(JSScript* script)
{
    return script->compartment();
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
    return JS_DefineProperty(cx, obj, prop, atom, JSPROP_READONLY | JSPROP_PERMANENT);
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
        RootedFunction fun(cx, DefineFunction(cx, obj, id, fs->call, fs->nargs,
                                              fs->flags | JSPROP_RESOLVING));
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
js::GetBuiltinClass(JSContext* cx, HandleObject obj, ESClass* cls)
{
    if (MOZ_UNLIKELY(obj->is<ProxyObject>()))
        return Proxy::getBuiltinClass(cx, obj, cls);

    if (obj->is<PlainObject>() || obj->is<UnboxedPlainObject>())
        *cls = ESClass::Object;
    else if (obj->is<ArrayObject>())
        *cls = ESClass::Array;
    else if (obj->is<NumberObject>())
        *cls = ESClass::Number;
    else if (obj->is<StringObject>())
        *cls = ESClass::String;
    else if (obj->is<BooleanObject>())
        *cls = ESClass::Boolean;
    else if (obj->is<RegExpObject>())
        *cls = ESClass::RegExp;
    else if (obj->is<ArrayBufferObject>())
        *cls = ESClass::ArrayBuffer;
    else if (obj->is<SharedArrayBufferObject>())
        *cls = ESClass::SharedArrayBuffer;
    else if (obj->is<DateObject>())
        *cls = ESClass::Date;
    else if (obj->is<SetObject>())
        *cls = ESClass::Set;
    else if (obj->is<MapObject>())
        *cls = ESClass::Map;
    else if (obj->is<PromiseObject>())
        *cls = ESClass::Promise;
    else if (obj->is<MapIteratorObject>())
        *cls = ESClass::MapIterator;
    else if (obj->is<SetIteratorObject>())
        *cls = ESClass::SetIterator;
    else if (obj->is<ArgumentsObject>())
        *cls = ESClass::Arguments;
    else if (obj->is<ErrorObject>())
        *cls = ESClass::Error;
    else
        *cls = ESClass::Other;

    return true;
}

JS_FRIEND_API(const char*)
js::ObjectClassName(JSContext* cx, HandleObject obj)
{
    assertSameCompartment(cx, obj);
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
    return obj->staticPrototype();
}

JS_FRIEND_API(void)
js::AssertSameCompartment(JSContext* cx, JSObject* obj)
{
    assertSameCompartment(cx, obj);
}

JS_FRIEND_API(void)
js::AssertSameCompartment(JSContext* cx, JS::HandleValue v)
{
    assertSameCompartment(cx, v);
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
    obj->runtimeFromActiveCooperatingThread()->lastAnimationTime = timeNow;
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
    assertSameCompartment(cx, id);

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
    assertSameCompartment(cx, obj);

    if (IsProxy(obj))
        return JS_GetPrototype(cx, obj, proto);

    proto.set(reinterpret_cast<const shadow::Object*>(obj.get())->group->proto);
    return true;
}

JS_FRIEND_API(JSObject*)
js::GetStaticPrototype(JSObject* obj)
{
    MOZ_ASSERT(obj->hasStaticPrototype());
    return obj->staticPrototype();
}

JS_FRIEND_API(bool)
js::GetOriginalEval(JSContext* cx, HandleObject scope, MutableHandleObject eval)
{
    assertSameCompartment(cx, scope);
    Rooted<GlobalObject*> global(cx, &scope->global());
    return GlobalObject::getOrCreateEval(cx, global, eval);
}

JS_FRIEND_API(void)
js::SetReservedSlotWithBarrier(JSObject* obj, size_t slot, const js::Value& value)
{
    if (IsProxy(obj))
        obj->as<ProxyObject>().setReservedSlot(slot, value);
    else
        obj->as<NativeObject>().setSlot(slot, value);
}

void
js::SetPreserveWrapperCallback(JSContext* cx, PreserveWrapperCallback callback)
{
    cx->runtime()->preserveWrapperCallback = callback;
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

JS_FRIEND_API(JSObject*)
JS_NewDeadWrapper(JSContext* cx, JSObject* origObj)
{
    return NewDeadProxyObject(cx, origObj);
}

JS_FRIEND_API(bool)
JS_IsScriptSourceObject(JSObject* obj)
{
    return obj->is<ScriptSourceObject>();
}

void
js::TraceWeakMaps(WeakMapTracer* trc)
{
    WeakMapBase::traceAllMappings(trc);
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
        JSObject* obj = comp->unsafeUnbarrieredMaybeGlobal();
        if (!obj || !JS::ObjectIsMarkedGray(obj))
            return false;
    }
    return true;
}

JS_FRIEND_API(bool)
js::IsObjectZoneSweepingOrCompacting(JSObject* obj)
{
    MOZ_ASSERT(obj);
    return MaybeForwarded(obj)->zone()->isGCSweepingOrCompacting();
}

namespace {
struct VisitGrayCallbackFunctor {
    GCThingCallback callback_;
    void* closure_;
    VisitGrayCallbackFunctor(GCThingCallback callback, void* closure)
      : callback_(callback), closure_(closure)
    {}

    template <class T>
    void operator()(T tp) const {
        if ((*tp)->isMarkedGray())
            callback_(closure_, JS::GCCellPtr(*tp));
    }
};
} // namespace (anonymous)

JS_FRIEND_API(void)
js::VisitGrayWrapperTargets(Zone* zone, GCThingCallback callback, void* closure)
{
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
        for (JSCompartment::WrapperEnum e(comp); !e.empty(); e.popFront())
            e.front().mutableKey().applyToWrapped(VisitGrayCallbackFunctor(callback, closure));
    }
}

JS_FRIEND_API(JSObject*)
js::GetWeakmapKeyDelegate(JSObject* key)
{
    if (JSWeakmapKeyDelegateOp op = key->getClass()->extWeakmapKeyDelegateOp())
        return op(key);
    return nullptr;
}

JS_FRIEND_API(JSLinearString*)
js::StringToLinearStringSlow(JSContext* cx, JSString* str)
{
    return str->ensureLinear(cx);
}

JS_FRIEND_API(void)
JS_SetAccumulateTelemetryCallback(JSContext* cx, JSAccumulateTelemetryDataCallback callback)
{
    cx->runtime()->setTelemetryCallback(cx->runtime(), callback);
}

JS_FRIEND_API(void)
JS_SetSetUseCounterCallback(JSContext* cx, JSSetUseCounterCallback callback)
{
    cx->runtime()->setUseCounterCallback(cx->runtime(), callback);
}

JS_FRIEND_API(JSObject*)
JS_CloneObject(JSContext* cx, HandleObject obj, HandleObject protoArg)
{
    // |obj| might be in a different compartment.
    assertSameCompartment(cx, protoArg);
    Rooted<TaggedProto> proto(cx, TaggedProto(protoArg.get()));
    return CloneObject(cx, obj, proto);
}

#ifdef DEBUG

// We don't want jsfriendapi.h to depend on GenericPrinter,
// so these functions are declared directly in the cpp.

namespace js {

extern JS_FRIEND_API(void)
DumpString(JSString* str, js::GenericPrinter& out);

extern JS_FRIEND_API(void)
DumpAtom(JSAtom* atom, js::GenericPrinter& out);

extern JS_FRIEND_API(void)
DumpObject(JSObject* obj, js::GenericPrinter& out);

extern JS_FRIEND_API(void)
DumpChars(const char16_t* s, size_t n, js::GenericPrinter& out);

extern JS_FRIEND_API(void)
DumpValue(const JS::Value& val, js::GenericPrinter& out);

extern JS_FRIEND_API(void)
DumpId(jsid id, js::GenericPrinter& out);

extern JS_FRIEND_API(void)
DumpInterpreterFrame(JSContext* cx, js::GenericPrinter& out, InterpreterFrame* start = nullptr);

} // namespace js

JS_FRIEND_API(void)
js::DumpString(JSString* str, js::GenericPrinter& out)
{
    str->dump(out);
}

JS_FRIEND_API(void)
js::DumpAtom(JSAtom* atom, js::GenericPrinter& out)
{
    atom->dump(out);
}

JS_FRIEND_API(void)
js::DumpChars(const char16_t* s, size_t n, js::GenericPrinter& out)
{
    out.printf("char16_t * (%p) = ", (void*) s);
    JSString::dumpChars(s, n, out);
    out.putChar('\n');
}

JS_FRIEND_API(void)
js::DumpObject(JSObject* obj, js::GenericPrinter& out)
{
    if (!obj) {
        out.printf("NULL\n");
        return;
    }
    obj->dump(out);
}

JS_FRIEND_API(void)
js::DumpString(JSString* str, FILE* fp)
{
    Fprinter out(fp);
    js::DumpString(str, out);
}

JS_FRIEND_API(void)
js::DumpAtom(JSAtom* atom, FILE* fp)
{
    Fprinter out(fp);
    js::DumpAtom(atom, out);
}

JS_FRIEND_API(void)
js::DumpChars(const char16_t* s, size_t n, FILE* fp)
{
    Fprinter out(fp);
    js::DumpChars(s, n, out);
}

JS_FRIEND_API(void)
js::DumpObject(JSObject* obj, FILE* fp)
{
    Fprinter out(fp);
    js::DumpObject(obj, out);
}

JS_FRIEND_API(void)
js::DumpId(jsid id, FILE* fp)
{
    Fprinter out(fp);
    js::DumpId(id, out);
}

JS_FRIEND_API(void)
js::DumpValue(const JS::Value& val, FILE* fp) {
    Fprinter out(fp);
    js::DumpValue(val, out);
}

JS_FRIEND_API(void)
js::DumpString(JSString* str) {
    DumpString(str, stderr);
}
JS_FRIEND_API(void)
js::DumpAtom(JSAtom* atom) {
    DumpAtom(atom, stderr);
}
JS_FRIEND_API(void)
js::DumpObject(JSObject* obj) {
    DumpObject(obj, stderr);
}
JS_FRIEND_API(void)
js::DumpChars(const char16_t* s, size_t n) {
    DumpChars(s, n, stderr);
}
JS_FRIEND_API(void)
js::DumpValue(const JS::Value& val) {
    DumpValue(val, stderr);
}
JS_FRIEND_API(void)
js::DumpId(jsid id) {
    DumpId(id, stderr);
}
JS_FRIEND_API(void)
js::DumpInterpreterFrame(JSContext* cx, InterpreterFrame* start)
{
    Fprinter out(stderr);
    DumpInterpreterFrame(cx, out, start);
}
JS_FRIEND_API(bool)
js::DumpPC(JSContext* cx) {
    return DumpPC(cx, stdout);
}
JS_FRIEND_API(bool)
js::DumpScript(JSContext* cx, JSScript* scriptArg)
{
    return DumpScript(cx, scriptArg, stdout);
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
static JS::UniqueChars
MOZ_FORMAT_PRINTF(3, 4)
sprintf_append(JSContext* cx, JS::UniqueChars&& buf, const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    JS::UniqueChars result = JS_vsprintf_append(Move(buf), fmt, ap);
    va_end(ap);

    if (!result) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    return result;
}

static JS::UniqueChars
FormatFrame(JSContext* cx, const FrameIter& iter, JS::UniqueChars&& inBuf, int num,
            bool showArgs, bool showLocals, bool showThisProps)
{
    MOZ_ASSERT(!cx->isExceptionPending());
    RootedScript script(cx, iter.script());
    jsbytecode* pc = iter.pc();

    RootedObject envChain(cx, iter.environmentChain(cx));
    JSAutoCompartment ac(cx, envChain);

    const char* filename = script->filename();
    unsigned lineno = PCToLineNumber(script, pc);
    RootedFunction fun(cx, iter.maybeCallee(cx));
    RootedString funname(cx);
    if (fun)
        funname = fun->displayAtom();

    RootedValue thisVal(cx);
    if (iter.hasUsableAbstractFramePtr() &&
        iter.isFunctionFrame() &&
        fun && !fun->isArrow() && !fun->isDerivedClassConstructor() &&
        !(fun->isBoundFunction() && iter.isConstructing()))
    {
        if (!GetFunctionThis(cx, iter.abstractFramePtr(), &thisVal))
            return nullptr;
    }

    // print the frame number and function name
    JS::UniqueChars buf(Move(inBuf));
    if (funname) {
        JSAutoByteString funbytes;
        char* str = funbytes.encodeLatin1(cx, funname);
        if (!str)
            return nullptr;
        buf = sprintf_append(cx, Move(buf), "%d %s(", num, str);
    } else if (fun) {
        buf = sprintf_append(cx, Move(buf), "%d anonymous(", num);
    } else {
        buf = sprintf_append(cx, Move(buf), "%d <TOP LEVEL>", num);
    }
    if (!buf)
        return nullptr;

    if (showArgs && iter.hasArgs()) {
        PositionalFormalParameterIter fi(script);
        bool first = true;
        for (unsigned i = 0; i < iter.numActualArgs(); i++) {
            RootedValue arg(cx);
            if (i < iter.numFormalArgs() && fi.closedOver()) {
                arg = iter.callObj(cx).aliasedBinding(fi);
            } else if (iter.hasUsableAbstractFramePtr()) {
                if (script->analyzedArgsUsage() &&
                    script->argsObjAliasesFormals() &&
                    iter.hasArgsObj())
                {
                    arg = iter.argsObj().arg(i);
                } else {
                    arg = iter.unaliasedActual(i, DONT_CHECK_ALIASING);
                }
            } else {
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
                MOZ_ASSERT(fi.argumentSlot() == i);
                if (!fi.isDestructured()) {
                    name = nameBytes.encodeLatin1(cx, fi.name());
                    if (!name)
                        return nullptr;
                } else {
                    name = "(destructured parameter)";
                }
                fi++;
            }

            if (value) {
                buf = sprintf_append(cx, Move(buf), "%s%s%s%s%s%s",
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
                buf = sprintf_append(cx, Move(buf),
                                     "    <Failed to get argument while inspecting stack frame>\n");
                if (!buf)
                    return nullptr;

            }
        }
    }

    // print filename and line number
    buf = sprintf_append(cx, Move(buf), "%s [\"%s\":%d]\n",
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
                buf = sprintf_append(cx, Move(buf), "    this = %s\n", str);
            } else {
                buf = sprintf_append(cx, Move(buf), "    <failed to get 'this' value>\n");
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
                buf = sprintf_append(cx, Move(buf),
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
                buf = sprintf_append(cx, Move(buf), "    this.%s = %s%s%s\n",
                                     name,
                                     v.isString() ? "\"" : "",
                                     value,
                                     v.isString() ? "\"" : "");
            } else {
                buf = sprintf_append(cx, Move(buf),
                                     "    <Failed to format values while inspecting stack frame>\n");
            }
            if (!buf)
                return nullptr;
        }
    }

    MOZ_ASSERT(!cx->isExceptionPending());
    return buf;
}

static JS::UniqueChars
FormatWasmFrame(JSContext* cx, const FrameIter& iter, JS::UniqueChars&& inBuf, int num)
{
    JSAtom* functionDisplayAtom = iter.functionDisplayAtom();
    UniqueChars nameStr;
    if (functionDisplayAtom)
        nameStr = StringToNewUTF8CharsZ(cx, *functionDisplayAtom);

    JS::UniqueChars buf = sprintf_append(cx, Move(inBuf), "%d %s()",
                                         num,
                                         nameStr ? nameStr.get() : "<wasm-function>");
    if (!buf)
        return nullptr;
    const char* filename = iter.filename();
    uint32_t lineno = iter.computeLine();
    buf = sprintf_append(cx, Move(buf), " [\"%s\":%d]\n",
                         filename ? filename : "<unknown>",
                         lineno);

    MOZ_ASSERT(!cx->isExceptionPending());
    return buf;
}

JS_FRIEND_API(JS::UniqueChars)
JS::FormatStackDump(JSContext* cx, JS::UniqueChars&& inBuf, bool showArgs, bool showLocals,
                    bool showThisProps)
{
    int num = 0;

    JS::UniqueChars buf(Move(inBuf));
    for (AllFramesIter i(cx); !i.done(); ++i) {
        if (i.hasScript())
            buf = FormatFrame(cx, i, Move(buf), num, showArgs, showLocals, showThisProps);
        else
            buf = FormatWasmFrame(cx, i, Move(buf), num);
        if (!buf)
            return nullptr;
        num++;
    }

    if (!num)
        buf = JS_sprintf_append(Move(buf), "JavaScript stack is empty\n");

    return buf;
}

extern JS_FRIEND_API(bool)
JS::ForceLexicalInitialization(JSContext *cx, HandleObject obj)
{
    AssertHeapIsIdle();
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);

    bool initializedAny = false;
    NativeObject* nobj = &obj->as<NativeObject>();

    for (Shape::Range<NoGC> r(nobj->lastProperty()); !r.empty(); r.popFront()) {
        Shape* s = &r.front();
        Value v = nobj->getSlot(s->slot());
        if (s->isDataProperty() && v.isMagic() && v.whyMagic() == JS_UNINITIALIZED_LEXICAL) {
            nobj->setSlot(s->slot(), UndefinedValue());
            initializedAny = true;
        }

    }
    return initializedAny;
}

extern JS_FRIEND_API(int)
JS::IsGCPoisoning()
{
#ifdef JS_GC_POISONING
    static bool disablePoison = bool(getenv("JSGC_DISABLE_POISONING"));
    return !disablePoison;
#else
    return false;
#endif
}

struct DumpHeapTracer : public JS::CallbackTracer, public WeakMapTracer
{
    const char* prefix;
    FILE* output;

    DumpHeapTracer(FILE* fp, JSContext* cx)
      : JS::CallbackTracer(cx, DoNotTraceWeakMaps),
        js::WeakMapTracer(cx->runtime()), prefix(""), output(fp)
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
    if (cell->isMarkedBlack())
        return 'B';
    if (cell->isMarkedGray())
        return 'G';
    if (cell->isMarkedAny())
        return 'X';
    return 'W';
}

static void
DumpHeapVisitZone(JSRuntime* rt, void* data, Zone* zone)
{
    DumpHeapTracer* dtrc = static_cast<DumpHeapTracer*>(data);
    fprintf(dtrc->output, "# zone %p\n", (void*)zone);
}

static void
DumpHeapVisitCompartment(JSContext* cx, void* data, JSCompartment* comp)
{
    char name[1024];
    if (cx->runtime()->compartmentNameCallback)
        (*cx->runtime()->compartmentNameCallback)(cx, comp, name, sizeof(name));
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
            unsigned(arena->getAllocKind()), unsigned(thingSize));
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
js::DumpHeap(JSContext* cx, FILE* fp, js::DumpHeapNurseryBehaviour nurseryBehaviour)
{
    if (nurseryBehaviour == js::CollectNurseryBeforeDump)
        EvictAllNurseries(cx->runtime(), JS::gcreason::API);

    DumpHeapTracer dtrc(fp, cx);

    fprintf(dtrc.output, "# Roots.\n");
    {
        JSRuntime* rt = cx->runtime();
        js::gc::AutoPrepareForTracing prep(cx);
        gcstats::AutoPhase ap(rt->gc.stats(), gcstats::PhaseKind::TRACE_HEAP);
        rt->gc.traceRuntime(&dtrc, prep.session());
    }

    fprintf(dtrc.output, "# Weak maps.\n");
    WeakMapBase::traceAllMappings(&dtrc);

    fprintf(dtrc.output, "==========\n");

    dtrc.prefix = "> ";
    IterateHeapUnbarriered(cx, &dtrc,
                                                   DumpHeapVisitZone,
                                                   DumpHeapVisitCompartment,
                                                   DumpHeapVisitArena,
                                                   DumpHeapVisitCell);

    fflush(dtrc.output);
}

JS_FRIEND_API(void)
js::SetActivityCallback(JSContext* cx, ActivityCallback cb, void* arg)
{
    cx->activityCallback = cb;
    cx->activityCallbackArg = arg;
}

JS_FRIEND_API(void)
JS::NotifyGCRootsRemoved(JSContext* cx)
{
    cx->runtime()->gc.notifyRootsRemoved();
}

JS_FRIEND_API(JSCompartment*)
js::GetAnyCompartmentInZone(JS::Zone* zone)
{
    CompartmentsInZoneIter comp(zone);
    MOZ_ASSERT(!comp.done());
    return comp.get();
}

void
JS::ObjectPtr::finalize(JSRuntime* rt)
{
    if (IsIncrementalBarrierNeeded(rt->activeContextFromOwnThread()))
        IncrementalPreWriteBarrier(value);
    value = nullptr;
}

void
JS::ObjectPtr::finalize(JSContext* cx)
{
    finalize(cx->runtime());
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
    JS::TraceEdge(trc, &value, name);
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
js::SetDOMCallbacks(JSContext* cx, const DOMCallbacks* callbacks)
{
    cx->runtime()->DOMcallbacks = callbacks;
}

JS_FRIEND_API(const DOMCallbacks*)
js::GetDOMCallbacks(JSContext* cx)
{
    return cx->runtime()->DOMcallbacks;
}

static const void* gDOMProxyHandlerFamily = nullptr;
static DOMProxyShadowsCheck gDOMProxyShadowsCheck;

JS_FRIEND_API(void)
js::SetDOMProxyInformation(const void* domProxyHandlerFamily,
                           DOMProxyShadowsCheck domProxyShadowsCheck)
{
    gDOMProxyHandlerFamily = domProxyHandlerFamily;
    gDOMProxyShadowsCheck = domProxyShadowsCheck;
}

const void*
js::GetDOMProxyHandlerFamily()
{
    return gDOMProxyHandlerFamily;
}

DOMProxyShadowsCheck
js::GetDOMProxyShadowsCheck()
{
    return gDOMProxyShadowsCheck;
}

static XrayJitInfo* gXrayJitInfo = nullptr;

JS_FRIEND_API(void)
js::SetXrayJitInfo(XrayJitInfo* info)
{
    gXrayJitInfo = info;
}

XrayJitInfo*
js::GetXrayJitInfo()
{
    return gXrayJitInfo;
}

bool
js::detail::IdMatchesAtom(jsid id, JSAtom* atom)
{
    return id == INTERNED_STRING_TO_JSID(nullptr, atom);
}

bool
js::detail::IdMatchesAtom(jsid id, JSString* atom)
{
    return id == INTERNED_STRING_TO_JSID(nullptr, atom);
}

JS_FRIEND_API(void)
js::PrepareScriptEnvironmentAndInvoke(JSContext* cx, HandleObject scope, ScriptEnvironmentPreparer::Closure& closure)
{
    MOZ_ASSERT(!cx->isExceptionPending());

    MOZ_RELEASE_ASSERT(cx->runtime()->scriptEnvironmentPreparer,
                       "Embedding needs to set a scriptEnvironmentPreparer callback");

    cx->runtime()->scriptEnvironmentPreparer->invoke(scope, closure);
}

JS_FRIEND_API(void)
js::SetScriptEnvironmentPreparer(JSContext* cx, ScriptEnvironmentPreparer* preparer)
{
    cx->runtime()->scriptEnvironmentPreparer = preparer;
}

JS_FRIEND_API(void)
js::SetCTypesActivityCallback(JSContext* cx, CTypesActivityCallback cb)
{
    cx->runtime()->ctypesActivityCallback = cb;
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
js::SetAllocationMetadataBuilder(JSContext* cx, const AllocationMetadataBuilder *callback)
{
    cx->compartment()->setAllocationMetadataBuilder(callback);
}

JS_FRIEND_API(JSObject*)
js::GetAllocationMetadata(JSObject* obj)
{
    ObjectWeakMap* map = obj->compartment()->objectMetadataTable;
    if (map)
        return map->lookup(obj);
    return nullptr;
}

JS_FRIEND_API(bool)
js::ReportIsNotFunction(JSContext* cx, HandleValue v)
{
    assertSameCompartment(cx, v);
    return ReportIsNotFunction(cx, v, -1);
}

#ifdef DEBUG
bool
js::HasObjectMovedOp(JSObject* obj) {
    return !!GetObjectClass(obj)->extObjectMovedOp();
}
#endif

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
js::SetWindowProxyClass(JSContext* cx, const js::Class* clasp)
{
    MOZ_ASSERT(!cx->runtime()->maybeWindowProxyClass());
    cx->runtime()->setWindowProxyClass(clasp);
}

JS_FRIEND_API(void)
js::SetWindowProxy(JSContext* cx, HandleObject global, HandleObject windowProxy)
{
    AssertHeapIsIdle();
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
js::detail::ToWindowProxyIfWindowSlow(JSObject* obj)
{
    if (JSObject* windowProxy = obj->as<GlobalObject>().maybeWindowProxy())
        return windowProxy;
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

AutoAssertNoContentJS::AutoAssertNoContentJS(JSContext* cx)
  : context_(cx),
    prevAllowContentJS_(cx->runtime()->allowContentJS_)
{
    cx->runtime()->allowContentJS_ = false;
}

AutoAssertNoContentJS::~AutoAssertNoContentJS()
{
    context_->runtime()->allowContentJS_ = prevAllowContentJS_;
}

JS_FRIEND_API(void)
js::EnableAccessValidation(JSContext* cx, bool enabled)
{
    cx->enableAccessValidation = enabled;
}

JS_FRIEND_API(void)
js::SetCompartmentValidAccessPtr(JSContext* cx, JS::HandleObject global, bool* accessp)
{
    global->compartment()->setValidAccessPtr(accessp);
}

JS_FRIEND_API(void)
js::SetCooperativeYieldCallback(JSContext* cx, YieldCallback callback)
{
    cx->setYieldCallback(callback);
}

JS_FRIEND_API(bool)
js::SystemZoneAvailable(JSContext* cx)
{
    CooperatingContext& owner = cx->runtime()->gc.systemZoneGroup->ownerContext();
    return owner.context() == nullptr;
}

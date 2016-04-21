/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SavedStacks.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Move.h"

#include <algorithm>
#include <math.h>

#include "jsapi.h"
#include "jscompartment.h"
#include "jsfriendapi.h"
#include "jshashutil.h"
#include "jsmath.h"
#include "jsnum.h"
#include "jsscript.h"

#include "gc/Marking.h"
#include "gc/Rooting.h"
#include "js/Vector.h"
#include "vm/Debugger.h"
#include "vm/SavedFrame.h"
#include "vm/StringBuffer.h"
#include "vm/Time.h"
#include "vm/WrapperObject.h"

#include "jscntxtinlines.h"

#include "vm/NativeObject-inl.h"
#include "vm/Stack-inl.h"

using mozilla::AddToHash;
using mozilla::DebugOnly;
using mozilla::HashString;
using mozilla::Maybe;
using mozilla::Move;
using mozilla::Nothing;
using mozilla::Some;

namespace js {

/**
 * Maximum number of saved frames returned for an async stack.
 */
const unsigned ASYNC_STACK_MAX_FRAME_COUNT = 60;

/* static */ Maybe<LiveSavedFrameCache::FramePtr>
LiveSavedFrameCache::getFramePtr(FrameIter& iter)
{
    if (iter.hasUsableAbstractFramePtr())
        return Some(FramePtr(iter.abstractFramePtr()));

    if (iter.isPhysicalIonFrame())
        return Some(FramePtr(iter.physicalIonFrame()));

    return Nothing();
}

/* static */ void
LiveSavedFrameCache::trace(LiveSavedFrameCache* cache, JSTracer* trc)
{
    if (!cache->initialized())
        return;

    for (auto* entry = cache->frames->begin(); entry < cache->frames->end(); entry++) {
        TraceEdge(trc,
                  &entry->savedFrame,
                  "LiveSavedFrameCache::frames SavedFrame");
    }
}

bool
LiveSavedFrameCache::insert(JSContext* cx, FramePtr& framePtr, jsbytecode* pc,
                            HandleSavedFrame savedFrame)
{
    MOZ_ASSERT(initialized());

    if (!frames->emplaceBack(framePtr, pc, savedFrame)) {
        ReportOutOfMemory(cx);
        return false;
    }

    // Safe to dereference the cache key because the stack frames are still
    // live. After this point, they should never be dereferenced again.
    if (framePtr.is<AbstractFramePtr>())
        framePtr.as<AbstractFramePtr>().setHasCachedSavedFrame();
    else
        framePtr.as<jit::CommonFrameLayout*>()->setHasCachedSavedFrame();

    return true;
}

void
LiveSavedFrameCache::find(JSContext* cx, FrameIter& frameIter, MutableHandleSavedFrame frame) const
{
    MOZ_ASSERT(initialized());
    MOZ_ASSERT(!frameIter.done());
    MOZ_ASSERT(frameIter.hasCachedSavedFrame());

    Maybe<FramePtr> maybeFramePtr = getFramePtr(frameIter);
    MOZ_ASSERT(maybeFramePtr.isSome());

    FramePtr framePtr(*maybeFramePtr);
    jsbytecode* pc = frameIter.pc();
    size_t numberStillValid = 0;

    frame.set(nullptr);
    for (auto* p = frames->begin(); p < frames->end(); p++) {
        numberStillValid++;
        if (framePtr == p->framePtr && pc == p->pc) {
            frame.set(p->savedFrame);
            break;
        }
    }

    if (!frame) {
        frames->clear();
        return;
    }

    MOZ_ASSERT(0 < numberStillValid && numberStillValid <= frames->length());

    if (frame->compartment() != cx->compartment()) {
        frame.set(nullptr);
        numberStillValid--;
    }

    // Everything after the cached SavedFrame are stale younger frames we have
    // since popped.
    frames->shrinkBy(frames->length() - numberStillValid);
}

struct SavedFrame::Lookup {
    Lookup(JSAtom* source, uint32_t line, uint32_t column,
           JSAtom* functionDisplayName, JSAtom* asyncCause, SavedFrame* parent,
           JSPrincipals* principals, Maybe<LiveSavedFrameCache::FramePtr> framePtr = Nothing(),
           jsbytecode* pc = nullptr, Activation* activation = nullptr)
      : source(source),
        line(line),
        column(column),
        functionDisplayName(functionDisplayName),
        asyncCause(asyncCause),
        parent(parent),
        principals(principals),
        framePtr(framePtr),
        pc(pc),
        activation(activation)
    {
        MOZ_ASSERT(source);
        MOZ_ASSERT_IF(framePtr.isSome(), pc);
        MOZ_ASSERT_IF(framePtr.isSome(), activation);
    }

    explicit Lookup(SavedFrame& savedFrame)
      : source(savedFrame.getSource()),
        line(savedFrame.getLine()),
        column(savedFrame.getColumn()),
        functionDisplayName(savedFrame.getFunctionDisplayName()),
        asyncCause(savedFrame.getAsyncCause()),
        parent(savedFrame.getParent()),
        principals(savedFrame.getPrincipals()),
        framePtr(Nothing()),
        pc(nullptr),
        activation(nullptr)
    {
        MOZ_ASSERT(source);
    }

    JSAtom*       source;
    uint32_t      line;
    uint32_t      column;
    JSAtom*       functionDisplayName;
    JSAtom*       asyncCause;
    SavedFrame*   parent;
    JSPrincipals* principals;

    // These are used only by the LiveSavedFrameCache and not used for identity or
    // hashing.
    Maybe<LiveSavedFrameCache::FramePtr> framePtr;
    jsbytecode*                          pc;
    Activation*                          activation;

    void trace(JSTracer* trc) {
        TraceManuallyBarrieredEdge(trc, &source, "SavedFrame::Lookup::source");
        if (functionDisplayName) {
            TraceManuallyBarrieredEdge(trc, &functionDisplayName,
                                       "SavedFrame::Lookup::functionDisplayName");
        }
        if (asyncCause)
            TraceManuallyBarrieredEdge(trc, &asyncCause, "SavedFrame::Lookup::asyncCause");
        if (parent)
            TraceManuallyBarrieredEdge(trc, &parent, "SavedFrame::Lookup::parent");
    }
};

class MOZ_STACK_CLASS SavedFrame::AutoLookupVector : public JS::CustomAutoRooter {
  public:
    explicit AutoLookupVector(JSContext* cx)
      : JS::CustomAutoRooter(cx),
        lookups(cx)
    { }

    typedef Vector<Lookup, ASYNC_STACK_MAX_FRAME_COUNT> LookupVector;
    inline LookupVector* operator->() { return &lookups; }
    inline HandleLookup operator[](size_t i) { return HandleLookup(lookups[i]); }

  private:
    LookupVector lookups;

    virtual void trace(JSTracer* trc) {
        for (size_t i = 0; i < lookups.length(); i++)
            lookups[i].trace(trc);
    }
};

/* static */ HashNumber
SavedFrame::HashPolicy::hash(const Lookup& lookup)
{
    JS::AutoCheckCannotGC nogc;
    // Assume that we can take line mod 2^32 without losing anything of
    // interest.  If that assumption changes, we'll just need to start with 0
    // and add another overload of AddToHash with more arguments.
    return AddToHash(lookup.line,
                     lookup.column,
                     lookup.source,
                     lookup.functionDisplayName,
                     lookup.asyncCause,
                     SavedFramePtrHasher::hash(lookup.parent),
                     JSPrincipalsPtrHasher::hash(lookup.principals));
}

/* static */ bool
SavedFrame::HashPolicy::match(SavedFrame* existing, const Lookup& lookup)
{
    if (existing->getLine() != lookup.line)
        return false;

    if (existing->getColumn() != lookup.column)
        return false;

    if (existing->getParent() != lookup.parent)
        return false;

    if (existing->getPrincipals() != lookup.principals)
        return false;

    JSAtom* source = existing->getSource();
    if (source != lookup.source)
        return false;

    JSAtom* functionDisplayName = existing->getFunctionDisplayName();
    if (functionDisplayName != lookup.functionDisplayName)
        return false;

    JSAtom* asyncCause = existing->getAsyncCause();
    if (asyncCause != lookup.asyncCause)
        return false;

    return true;
}

/* static */ void
SavedFrame::HashPolicy::rekey(Key& key, const Key& newKey)
{
    key = newKey;
}

/* static */ bool
SavedFrame::finishSavedFrameInit(JSContext* cx, HandleObject ctor, HandleObject proto)
{
    // The only object with the SavedFrame::class_ that doesn't have a source
    // should be the prototype.
    proto->as<NativeObject>().setReservedSlot(SavedFrame::JSSLOT_SOURCE, NullValue());

    return FreezeObject(cx, proto);
}

/* static */ const Class SavedFrame::class_ = {
    "SavedFrame",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(SavedFrame::JSSLOT_COUNT) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_SavedFrame) |
    JSCLASS_IS_ANONYMOUS,
    nullptr,                    // addProperty
    nullptr,                    // delProperty
    nullptr,                    // getProperty
    nullptr,                    // setProperty
    nullptr,                    // enumerate
    nullptr,                    // resolve
    nullptr,                    // mayResolve
    SavedFrame::finalize,       // finalize
    nullptr,                    // call
    nullptr,                    // hasInstance
    nullptr,                    // construct
    nullptr,                    // trace

    // ClassSpec
    {
        GenericCreateConstructor<SavedFrame::construct, 0, gc::AllocKind::FUNCTION>,
        GenericCreatePrototype,
        SavedFrame::staticFunctions,
        nullptr,
        SavedFrame::protoFunctions,
        SavedFrame::protoAccessors,
        SavedFrame::finishSavedFrameInit,
        ClassSpec::DontDefineConstructor
    }
};

/* static */ const JSFunctionSpec
SavedFrame::staticFunctions[] = {
    JS_FS_END
};

/* static */ const JSFunctionSpec
SavedFrame::protoFunctions[] = {
    JS_FN("constructor", SavedFrame::construct, 0, 0),
    JS_FN("toString", SavedFrame::toStringMethod, 0, 0),
    JS_FS_END
};

/* static */ const JSPropertySpec
SavedFrame::protoAccessors[] = {
    JS_PSG("source", SavedFrame::sourceProperty, 0),
    JS_PSG("line", SavedFrame::lineProperty, 0),
    JS_PSG("column", SavedFrame::columnProperty, 0),
    JS_PSG("functionDisplayName", SavedFrame::functionDisplayNameProperty, 0),
    JS_PSG("asyncCause", SavedFrame::asyncCauseProperty, 0),
    JS_PSG("asyncParent", SavedFrame::asyncParentProperty, 0),
    JS_PSG("parent", SavedFrame::parentProperty, 0),
    JS_PS_END
};

/* static */ void
SavedFrame::finalize(FreeOp* fop, JSObject* obj)
{
    JSPrincipals* p = obj->as<SavedFrame>().getPrincipals();
    if (p) {
        JSRuntime* rt = obj->runtimeFromMainThread();
        JS_DropPrincipals(rt, p);
    }
}

JSAtom*
SavedFrame::getSource()
{
    const Value& v = getReservedSlot(JSSLOT_SOURCE);
    JSString* s = v.toString();
    return &s->asAtom();
}

uint32_t
SavedFrame::getLine()
{
    const Value& v = getReservedSlot(JSSLOT_LINE);
    return v.toPrivateUint32();
}

uint32_t
SavedFrame::getColumn()
{
    const Value& v = getReservedSlot(JSSLOT_COLUMN);
    return v.toPrivateUint32();
}

JSAtom*
SavedFrame::getFunctionDisplayName()
{
    const Value& v = getReservedSlot(JSSLOT_FUNCTIONDISPLAYNAME);
    if (v.isNull())
        return nullptr;
    JSString* s = v.toString();
    return &s->asAtom();
}

JSAtom*
SavedFrame::getAsyncCause()
{
    const Value& v = getReservedSlot(JSSLOT_ASYNCCAUSE);
    if (v.isNull())
        return nullptr;
    JSString* s = v.toString();
    return &s->asAtom();
}

SavedFrame*
SavedFrame::getParent() const
{
    const Value& v = getReservedSlot(JSSLOT_PARENT);
    return v.isObject() ? &v.toObject().as<SavedFrame>() : nullptr;
}

JSPrincipals*
SavedFrame::getPrincipals()
{
    const Value& v = getReservedSlot(JSSLOT_PRINCIPALS);
    if (v.isUndefined())
        return nullptr;
    return static_cast<JSPrincipals*>(v.toPrivate());
}

void
SavedFrame::initSource(JSAtom* source)
{
    MOZ_ASSERT(source);
    initReservedSlot(JSSLOT_SOURCE, StringValue(source));
}

void
SavedFrame::initLine(uint32_t line)
{
    initReservedSlot(JSSLOT_LINE, PrivateUint32Value(line));
}

void
SavedFrame::initColumn(uint32_t column)
{
    initReservedSlot(JSSLOT_COLUMN, PrivateUint32Value(column));
}

void
SavedFrame::initPrincipals(JSPrincipals* principals)
{
    if (principals)
        JS_HoldPrincipals(principals);
    initPrincipalsAlreadyHeld(principals);
}

void
SavedFrame::initPrincipalsAlreadyHeld(JSPrincipals* principals)
{
    MOZ_ASSERT_IF(principals, principals->refcount > 0);
    initReservedSlot(JSSLOT_PRINCIPALS, PrivateValue(principals));
}

void
SavedFrame::initFunctionDisplayName(JSAtom* maybeName)
{
    initReservedSlot(JSSLOT_FUNCTIONDISPLAYNAME, maybeName ? StringValue(maybeName) : NullValue());
}

void
SavedFrame::initAsyncCause(JSAtom* maybeCause)
{
    initReservedSlot(JSSLOT_ASYNCCAUSE, maybeCause ? StringValue(maybeCause) : NullValue());
}

void
SavedFrame::initParent(SavedFrame* maybeParent)
{
    initReservedSlot(JSSLOT_PARENT, ObjectOrNullValue(maybeParent));
}

void
SavedFrame::initFromLookup(SavedFrame::HandleLookup lookup)
{
    initSource(lookup->source);
    initLine(lookup->line);
    initColumn(lookup->column);
    initFunctionDisplayName(lookup->functionDisplayName);
    initAsyncCause(lookup->asyncCause);
    initParent(lookup->parent);
    initPrincipals(lookup->principals);
}

/* static */ SavedFrame*
SavedFrame::create(JSContext* cx)
{
    RootedGlobalObject global(cx, cx->global());
    assertSameCompartment(cx, global);

    // Ensure that we don't try to capture the stack again in the
    // `SavedStacksMetadataCallback` for this new SavedFrame object, and
    // accidentally cause O(n^2) behavior.
    SavedStacks::AutoReentrancyGuard guard(cx->compartment()->savedStacks());

    RootedNativeObject proto(cx, GlobalObject::getOrCreateSavedFramePrototype(cx, global));
    if (!proto)
        return nullptr;
    assertSameCompartment(cx, proto);

    RootedObject frameObj(cx, NewObjectWithGivenProto(cx, &SavedFrame::class_, proto));
    if (!frameObj)
        return nullptr;

    return &frameObj->as<SavedFrame>();
}

bool
SavedFrame::isSelfHosted()
{
    JSAtom* source = getSource();
    return StringEqualsAscii(source, "self-hosted");
}

/* static */ bool
SavedFrame::construct(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                         "SavedFrame");
    return false;
}

static bool
SavedFrameSubsumedByCaller(JSContext* cx, HandleSavedFrame frame)
{
    auto subsumes = cx->runtime()->securityCallbacks->subsumes;
    if (!subsumes)
        return true;

    auto currentCompartmentPrincipals = cx->compartment()->principals();
    MOZ_ASSERT(!ReconstructedSavedFramePrincipals::is(currentCompartmentPrincipals));

    auto framePrincipals = frame->getPrincipals();

    // Handle SavedFrames that have been reconstructed from stacks in a heap
    // snapshot.
    if (framePrincipals == &ReconstructedSavedFramePrincipals::IsSystem)
        return cx->runningWithTrustedPrincipals();
    if (framePrincipals == &ReconstructedSavedFramePrincipals::IsNotSystem)
        return true;

    return subsumes(currentCompartmentPrincipals, framePrincipals);
}

// Return the first SavedFrame in the chain that starts with |frame| whose
// principals are subsumed by |principals|, according to |subsumes|. If there is
// no such frame, return nullptr. |skippedAsync| is set to true if any of the
// skipped frames had the |asyncCause| property set, otherwise it is explicitly
// set to false.
static SavedFrame*
GetFirstSubsumedFrame(JSContext* cx, HandleSavedFrame frame, JS::SavedFrameSelfHosted selfHosted,
                      bool& skippedAsync)
{
    skippedAsync = false;

    RootedSavedFrame rootedFrame(cx, frame);
    while (rootedFrame) {
        if ((selfHosted == JS::SavedFrameSelfHosted::Include || !rootedFrame->isSelfHosted()) &&
            SavedFrameSubsumedByCaller(cx, rootedFrame))
        {
            return rootedFrame;
        }

        if (rootedFrame->getAsyncCause())
            skippedAsync = true;

        rootedFrame = rootedFrame->getParent();
    }

    return nullptr;
}

JS_FRIEND_API(JSObject*)
GetFirstSubsumedSavedFrame(JSContext* cx, HandleObject savedFrame,
                           JS::SavedFrameSelfHosted selfHosted)
{
    if (!savedFrame)
        return nullptr;
    bool skippedAsync;
    RootedSavedFrame frame(cx, &savedFrame->as<SavedFrame>());
    return GetFirstSubsumedFrame(cx, frame, selfHosted, skippedAsync);
}

/* static */ bool
SavedFrame::checkThis(JSContext* cx, CallArgs& args, const char* fnName,
                      MutableHandleObject frame)
{
    const Value& thisValue = args.thisv();

    if (!thisValue.isObject()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_NONNULL_OBJECT, InformalValueTypeName(thisValue));
        return false;
    }

    JSObject* thisObject = CheckedUnwrap(&thisValue.toObject());
    if (!thisObject || !thisObject->is<SavedFrame>()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             SavedFrame::class_.name, fnName,
                             thisObject ? thisObject->getClass()->name : "object");
        return false;
    }

    // Check for SavedFrame.prototype, which has the same class as SavedFrame
    // instances, however doesn't actually represent a captured stack frame. It
    // is the only object that is<SavedFrame>() but doesn't have a source.
    if (thisObject->as<SavedFrame>().getReservedSlot(JSSLOT_SOURCE).isNull()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             SavedFrame::class_.name, fnName, "prototype object");
        return false;
    }

    // Now set "frame" to the actual object we were invoked in (which may be a
    // wrapper), not the unwrapped version.  Consumers will need to know what
    // that original object was, and will do principal checks as needed.
    frame.set(&thisValue.toObject());
    return true;
}

// Get the SavedFrame * from the current this value and handle any errors that
// might occur therein.
//
// These parameters must already exist when calling this macro:
//   - JSContext* cx
//   - unsigned   argc
//   - Value*     vp
//   - const char* fnName
// These parameters will be defined after calling this macro:
//   - CallArgs args
//   - Rooted<SavedFrame*> frame (will be non-null)
#define THIS_SAVEDFRAME(cx, argc, vp, fnName, args, frame)             \
    CallArgs args = CallArgsFromVp(argc, vp);                          \
    RootedObject frame(cx);                                            \
    if (!checkThis(cx, args, fnName, &frame))                          \
        return false;

} /* namespace js */

namespace JS {

namespace {

// It's possible that our caller is privileged (and hence would see the entire
// stack) but we're working with an SavedFrame object that was captured in
// unprivileged code.  If so, drop privileges down to its level.  The idea is
// that this way devtools code that's asking an exception object for a stack to
// display will end up with the stack the web developer would see via doing
// .stack in a web page, with Firefox implementation details excluded.
//
// We want callers to pass us the object they were actually passed, not an
// unwrapped form of it.  That way Xray access to SavedFrame objects should not
// be affected by AutoMaybeEnterFrameCompartment and the only things that will
// be affected will be cases in which privileged code works with some C++ object
// that then pokes at an unprivileged StackFrame it has on hand.
class MOZ_STACK_CLASS AutoMaybeEnterFrameCompartment
{
public:
    AutoMaybeEnterFrameCompartment(JSContext* cx,
                                   HandleObject obj
                                   MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        // Note that obj might be null here, since we're doing this
        // before UnwrapSavedFrame.
        if (obj && cx->compartment() != obj->compartment())
        {
            JSSubsumesOp subsumes = cx->runtime()->securityCallbacks->subsumes;
            if (subsumes && subsumes(cx->compartment()->principals(),
                                     obj->compartment()->principals()))
            {
                ac_.emplace(cx, obj);
            }
        }
    }

 private:
    Maybe<JSAutoCompartment> ac_;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

} // namespace

static inline js::SavedFrame*
UnwrapSavedFrame(JSContext* cx, HandleObject obj, SavedFrameSelfHosted selfHosted,
                 bool& skippedAsync)
{
    if (!obj)
        return nullptr;

    RootedObject savedFrameObj(cx, CheckedUnwrap(obj));
    if (!savedFrameObj)
        return nullptr;

    MOZ_ASSERT(js::SavedFrame::isSavedFrameAndNotProto(*savedFrameObj));
    js::RootedSavedFrame frame(cx, &savedFrameObj->as<js::SavedFrame>());
    return GetFirstSubsumedFrame(cx, frame, selfHosted, skippedAsync);
}

JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameSource(JSContext* cx, HandleObject savedFrame, MutableHandleString sourcep,
                    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */)
{
    AutoMaybeEnterFrameCompartment ac(cx, savedFrame);
    bool skippedAsync;
    js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, savedFrame, selfHosted, skippedAsync));
    if (!frame) {
        sourcep.set(cx->runtime()->emptyString);
        return SavedFrameResult::AccessDenied;
    }
    sourcep.set(frame->getSource());
    return SavedFrameResult::Ok;
}

JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameLine(JSContext* cx, HandleObject savedFrame, uint32_t* linep,
                  SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */)
{
    MOZ_ASSERT(linep);
    AutoMaybeEnterFrameCompartment ac(cx, savedFrame);
    bool skippedAsync;
    js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, savedFrame, selfHosted, skippedAsync));
    if (!frame) {
        *linep = 0;
        return SavedFrameResult::AccessDenied;
    }
    *linep = frame->getLine();
    return SavedFrameResult::Ok;
}

JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameColumn(JSContext* cx, HandleObject savedFrame, uint32_t* columnp,
                    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */)
{
    MOZ_ASSERT(columnp);
    AutoMaybeEnterFrameCompartment ac(cx, savedFrame);
    bool skippedAsync;
    js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, savedFrame, selfHosted, skippedAsync));
    if (!frame) {
        *columnp = 0;
        return SavedFrameResult::AccessDenied;
    }
    *columnp = frame->getColumn();
    return SavedFrameResult::Ok;
}

JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameFunctionDisplayName(JSContext* cx, HandleObject savedFrame, MutableHandleString namep,
                                 SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */)
{
    AutoMaybeEnterFrameCompartment ac(cx, savedFrame);
    bool skippedAsync;
    js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, savedFrame, selfHosted, skippedAsync));
    if (!frame) {
        namep.set(nullptr);
        return SavedFrameResult::AccessDenied;
    }
    namep.set(frame->getFunctionDisplayName());
    return SavedFrameResult::Ok;
}

JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameAsyncCause(JSContext* cx, HandleObject savedFrame, MutableHandleString asyncCausep,
                        SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */)
{
    AutoMaybeEnterFrameCompartment ac(cx, savedFrame);
    bool skippedAsync;
    js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, savedFrame, selfHosted, skippedAsync));
    if (!frame) {
        asyncCausep.set(nullptr);
        return SavedFrameResult::AccessDenied;
    }
    asyncCausep.set(frame->getAsyncCause());
    if (!asyncCausep && skippedAsync)
        asyncCausep.set(cx->names().Async);
    return SavedFrameResult::Ok;
}

JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameAsyncParent(JSContext* cx, HandleObject savedFrame, MutableHandleObject asyncParentp,
                         SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */)
{
    AutoMaybeEnterFrameCompartment ac(cx, savedFrame);
    bool skippedAsync;
    js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, savedFrame, selfHosted, skippedAsync));
    if (!frame) {
        asyncParentp.set(nullptr);
        return SavedFrameResult::AccessDenied;
    }
    js::RootedSavedFrame parent(cx, frame->getParent());

    // The current value of |skippedAsync| is not interesting, because we are
    // interested in whether we would cross any async parents to get from here
    // to the first subsumed parent frame instead.
    js::RootedSavedFrame subsumedParent(cx, GetFirstSubsumedFrame(cx, parent, selfHosted,
                                                                  skippedAsync));

    // Even if |parent| is not subsumed, we still want to return a pointer to it
    // rather than |subsumedParent| so it can pick up any |asyncCause| from the
    // inaccessible part of the chain.
    if (subsumedParent && (subsumedParent->getAsyncCause() || skippedAsync))
        asyncParentp.set(parent);
    else
        asyncParentp.set(nullptr);
    return SavedFrameResult::Ok;
}

JS_PUBLIC_API(SavedFrameResult)
GetSavedFrameParent(JSContext* cx, HandleObject savedFrame, MutableHandleObject parentp,
                    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */)
{
    AutoMaybeEnterFrameCompartment ac(cx, savedFrame);
    bool skippedAsync;
    js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, savedFrame, selfHosted, skippedAsync));
    if (!frame) {
        parentp.set(nullptr);
        return SavedFrameResult::AccessDenied;
    }
    js::RootedSavedFrame parent(cx, frame->getParent());

    // The current value of |skippedAsync| is not interesting, because we are
    // interested in whether we would cross any async parents to get from here
    // to the first subsumed parent frame instead.
    js::RootedSavedFrame subsumedParent(cx, GetFirstSubsumedFrame(cx, parent, selfHosted,
                                                                  skippedAsync));

    // Even if |parent| is not subsumed, we still want to return a pointer to it
    // rather than |subsumedParent| so it can pick up any |asyncCause| from the
    // inaccessible part of the chain.
    if (subsumedParent && !(subsumedParent->getAsyncCause() || skippedAsync))
        parentp.set(parent);
    else
        parentp.set(nullptr);
    return SavedFrameResult::Ok;
}

JS_PUBLIC_API(bool)
BuildStackString(JSContext* cx, HandleObject stack, MutableHandleString stringp, size_t indent)
{
    js::StringBuffer sb(cx);

    // Enter a new block to constrain the scope of possibly entering the stack's
    // compartment. This ensures that when we finish the StringBuffer, we are
    // back in the cx's original compartment, and fulfill our contract with
    // callers to place the output string in the cx's current compartment.
    {
        AutoMaybeEnterFrameCompartment ac(cx, stack);
        bool skippedAsync;
        js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, stack, SavedFrameSelfHosted::Exclude,
                                                        skippedAsync));
        if (!frame) {
            stringp.set(cx->runtime()->emptyString);
            return true;
        }

        js::RootedSavedFrame parent(cx);
        do {
            MOZ_ASSERT(SavedFrameSubsumedByCaller(cx, frame));
            MOZ_ASSERT(!frame->isSelfHosted());

            RootedString asyncCause(cx, frame->getAsyncCause());
            if (!asyncCause && skippedAsync)
                asyncCause.set(cx->names().Async);

            js::RootedAtom name(cx, frame->getFunctionDisplayName());
            if ((indent && !sb.appendN(' ', indent))
                || (asyncCause && (!sb.append(asyncCause) || !sb.append('*')))
                || (name && !sb.append(name))
                || !sb.append('@')
                || !sb.append(frame->getSource())
                || !sb.append(':')
                || !NumberValueToStringBuffer(cx, NumberValue(frame->getLine()), sb)
                || !sb.append(':')
                || !NumberValueToStringBuffer(cx, NumberValue(frame->getColumn()), sb)
                || !sb.append('\n'))
            {
                return false;
            }

            parent = frame->getParent();
            frame = js::GetFirstSubsumedFrame(cx, parent, SavedFrameSelfHosted::Exclude, skippedAsync);
        } while (frame);
    }

    JSString* str = sb.finishString();
    if (!str)
        return false;
    assertSameCompartment(cx, str);
    stringp.set(str);
    return true;
}

} /* namespace JS */

namespace js {

/* static */ bool
SavedFrame::sourceProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get source)", args, frame);
    RootedString source(cx);
    if (JS::GetSavedFrameSource(cx, frame, &source) == JS::SavedFrameResult::Ok) {
        if (!cx->compartment()->wrap(cx, &source))
            return false;
        args.rval().setString(source);
    } else {
        args.rval().setNull();
    }
    return true;
}

/* static */ bool
SavedFrame::lineProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get line)", args, frame);
    uint32_t line;
    if (JS::GetSavedFrameLine(cx, frame, &line) == JS::SavedFrameResult::Ok)
        args.rval().setNumber(line);
    else
        args.rval().setNull();
    return true;
}

/* static */ bool
SavedFrame::columnProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get column)", args, frame);
    uint32_t column;
    if (JS::GetSavedFrameColumn(cx, frame, &column) == JS::SavedFrameResult::Ok)
        args.rval().setNumber(column);
    else
        args.rval().setNull();
    return true;
}

/* static */ bool
SavedFrame::functionDisplayNameProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get functionDisplayName)", args, frame);
    RootedString name(cx);
    JS::SavedFrameResult result = JS::GetSavedFrameFunctionDisplayName(cx, frame, &name);
    if (result == JS::SavedFrameResult::Ok && name) {
        if (!cx->compartment()->wrap(cx, &name))
            return false;
        args.rval().setString(name);
    } else {
        args.rval().setNull();
    }
    return true;
}

/* static */ bool
SavedFrame::asyncCauseProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get asyncCause)", args, frame);
    RootedString asyncCause(cx);
    JS::SavedFrameResult result = JS::GetSavedFrameAsyncCause(cx, frame, &asyncCause);
    if (result == JS::SavedFrameResult::Ok && asyncCause) {
        if (!cx->compartment()->wrap(cx, &asyncCause))
            return false;
        args.rval().setString(asyncCause);
    } else {
        args.rval().setNull();
    }
    return true;
}

/* static */ bool
SavedFrame::asyncParentProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get asyncParent)", args, frame);
    RootedObject asyncParent(cx);
    (void) JS::GetSavedFrameAsyncParent(cx, frame, &asyncParent);
    if (!cx->compartment()->wrap(cx, &asyncParent))
        return false;
    args.rval().setObjectOrNull(asyncParent);
    return true;
}

/* static */ bool
SavedFrame::parentProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get parent)", args, frame);
    RootedObject parent(cx);
    (void) JS::GetSavedFrameParent(cx, frame, &parent);
    if (!cx->compartment()->wrap(cx, &parent))
        return false;
    args.rval().setObjectOrNull(parent);
    return true;
}

/* static */ bool
SavedFrame::toStringMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "toString", args, frame);
    RootedString string(cx);
    if (!JS::BuildStackString(cx, frame, &string))
        return false;
    args.rval().setString(string);
    return true;
}

bool
SavedStacks::init()
{
    mozilla::Array<uint64_t, 2> seed;
    GenerateXorShift128PlusSeed(seed);
    bernoulli.setRandomState(seed[0], seed[1]);

    if (!pcLocationMap.init())
        return false;

    return frames.init();
}

bool
SavedStacks::saveCurrentStack(JSContext* cx, MutableHandleSavedFrame frame, unsigned maxFrameCount)
{
    MOZ_ASSERT(initialized());
    assertSameCompartment(cx, this);

    if (creatingSavedFrame ||
        cx->isExceptionPending() ||
        !cx->global()->isStandardClassResolved(JSProto_Object))
    {
        frame.set(nullptr);
        return true;
    }

    FrameIter iter(cx, FrameIter::ALL_CONTEXTS, FrameIter::GO_THROUGH_SAVED);
    return insertFrames(cx, iter, frame, maxFrameCount);
}

bool
SavedStacks::copyAsyncStack(JSContext* cx, HandleObject asyncStack, HandleString asyncCause,
                            MutableHandleSavedFrame adoptedStack, unsigned maxFrameCount)
{
    MOZ_ASSERT(initialized());
    assertSameCompartment(cx, this);

    RootedObject asyncStackObj(cx, CheckedUnwrap(asyncStack));
    MOZ_ASSERT(asyncStackObj);
    MOZ_ASSERT(js::SavedFrame::isSavedFrameAndNotProto(*asyncStackObj));
    RootedSavedFrame frame(cx, &asyncStackObj->as<js::SavedFrame>());

    return adoptAsyncStack(cx, frame, asyncCause, adoptedStack, maxFrameCount);
}

void
SavedStacks::sweep(JSRuntime* rt)
{
    frames.sweep();
    sweepPCLocationMap();
}

void
SavedStacks::trace(JSTracer* trc)
{
    if (pcLocationMap.initialized()) {
        // Mark each of the source strings in our pc to location cache.
        for (PCLocationMap::Enum e(pcLocationMap); !e.empty(); e.popFront()) {
            LocationValue& loc = e.front().value();
            TraceEdge(trc, &loc.source, "SavedStacks::PCLocationMap's memoized script source name");
        }
    }
}

uint32_t
SavedStacks::count()
{
    MOZ_ASSERT(initialized());
    return frames.count();
}

void
SavedStacks::clear()
{
    frames.clear();
}

size_t
SavedStacks::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    return frames.sizeOfExcludingThis(mallocSizeOf);
}

bool
SavedStacks::insertFrames(JSContext* cx, FrameIter& iter, MutableHandleSavedFrame frame,
                          unsigned maxFrameCount)
{
    // In order to lookup a cached SavedFrame object, we need to have its parent
    // SavedFrame, which means we need to walk the stack from oldest frame to
    // youngest. However, FrameIter walks the stack from youngest frame to
    // oldest. The solution is to append stack frames to a vector as we walk the
    // stack with FrameIter, and then do a second pass through that vector in
    // reverse order after the traversal has completed and get or create the
    // SavedFrame objects at that time.
    //
    // To avoid making many copies of FrameIter (whose copy constructor is
    // relatively slow), we use a vector of `SavedFrame::Lookup` objects, which
    // only contain the FrameIter data we need. The `SavedFrame::Lookup`
    // objects are partially initialized with everything except their parent
    // pointers on the first pass, and then we fill in the parent pointers as we
    // return in the second pass.

    Activation* asyncActivation = nullptr;
    RootedSavedFrame asyncStack(cx, nullptr);
    RootedString asyncCause(cx, nullptr);
    bool parentIsInCache = false;
    RootedSavedFrame cachedFrame(cx, nullptr);

    // Accumulate the vector of Lookup objects in |stackChain|.
    SavedFrame::AutoLookupVector stackChain(cx);
    while (!iter.done()) {
        Activation& activation = *iter.activation();

        if (asyncActivation && asyncActivation != &activation) {
            // We found an async stack in the previous activation, and we
            // walked past the oldest frame of that activation, we're done.
            // However, we only want to use the async parent if it was
            // explicitly requested; if we got here otherwise, we have
            // a direct parent, which we prefer.
            if (asyncActivation->asyncCallIsExplicit())
                break;
            asyncActivation = nullptr;
        }

        if (!asyncActivation) {
            asyncStack = activation.asyncStack();
            if (asyncStack) {
                // While walking from the youngest to the oldest frame, we found
                // an activation that has an async stack set. We will use the
                // youngest frame of the async stack as the parent of the oldest
                // frame of this activation. We still need to iterate over other
                // frames in this activation before reaching the oldest frame.
                asyncCause = activation.asyncCause();
                asyncActivation = &activation;
            }
        }

        AutoLocationValueRooter location(cx);
        {
            AutoCompartment ac(cx, iter.compartment());
            if (!cx->compartment()->savedStacks().getLocation(cx, iter, &location))
                return false;
        }

        // The bit set means that the next older parent (frame, pc) pair *must*
        // be in the cache.
        if (maxFrameCount == 0)
            parentIsInCache = iter.hasCachedSavedFrame();

        auto displayAtom = iter.isNonEvalFunctionFrame() ? iter.functionDisplayAtom() : nullptr;
        if (!stackChain->emplaceBack(location->source,
                                     location->line,
                                     location->column,
                                     displayAtom,
                                     nullptr,
                                     nullptr,
                                     iter.compartment()->principals(),
                                     LiveSavedFrameCache::getFramePtr(iter),
                                     iter.pc(),
                                     &activation))
        {
            ReportOutOfMemory(cx);
            return false;
        }

        ++iter;

        if (maxFrameCount == 1) {
            // The frame we just saved was the last one we were asked to save.
            // If we had an async stack, ensure we don't use any of its frames.
            asyncStack.set(nullptr);
            break;
        }

        if (parentIsInCache &&
            !iter.done() &&
            iter.hasCachedSavedFrame())
        {
            auto* cache = activation.getLiveSavedFrameCache(cx);
            if (!cache)
                return false;
            cache->find(cx, iter, &cachedFrame);
            if (cachedFrame)
                break;
        }

        // If maxFrameCount is zero there's no limit on the number of frames.
        if (maxFrameCount == 0)
            continue;

        maxFrameCount--;
    }

    // Limit the depth of the async stack, if any, and ensure that the
    // SavedFrame instances we use are stored in the same compartment as the
    // rest of the synchronous stack chain.
    RootedSavedFrame parentFrame(cx, cachedFrame);
    if (asyncStack && !adoptAsyncStack(cx, asyncStack, asyncCause, &parentFrame, maxFrameCount))
        return false;

    // Iterate through |stackChain| in reverse order and get or create the
    // actual SavedFrame instances.
    for (size_t i = stackChain->length(); i != 0; i--) {
        SavedFrame::HandleLookup lookup = stackChain[i-1];
        lookup->parent = parentFrame;
        parentFrame.set(getOrCreateSavedFrame(cx, lookup));
        if (!parentFrame)
            return false;

        if (maxFrameCount == 0 && lookup->framePtr && parentFrame != cachedFrame) {
            auto* cache = lookup->activation->getLiveSavedFrameCache(cx);
            if (!cache || !cache->insert(cx, *lookup->framePtr, lookup->pc, parentFrame))
                return false;
        }
    }

    frame.set(parentFrame);
    return true;
}

bool
SavedStacks::adoptAsyncStack(JSContext* cx, HandleSavedFrame asyncStack,
                             HandleString asyncCause,
                             MutableHandleSavedFrame adoptedStack,
                             unsigned maxFrameCount)
{
    RootedAtom asyncCauseAtom(cx, AtomizeString(cx, asyncCause));
    if (!asyncCauseAtom)
        return false;

    // If maxFrameCount is zero, the caller asked for an unlimited number of
    // stack frames, but async stacks are not limited by the available stack
    // memory, so we need to set an arbitrary limit when collecting them. We
    // still don't enforce an upper limit if the caller requested more frames.
    unsigned maxFrames = maxFrameCount > 0 ? maxFrameCount : ASYNC_STACK_MAX_FRAME_COUNT;

    // Accumulate the vector of Lookup objects in |stackChain|.
    SavedFrame::AutoLookupVector stackChain(cx);
    SavedFrame* currentSavedFrame = asyncStack;
    SavedFrame* firstSavedFrameParent = nullptr;
    for (unsigned i = 0; i < maxFrames && currentSavedFrame; i++) {
        if (!stackChain->emplaceBack(*currentSavedFrame)) {
            ReportOutOfMemory(cx);
            return false;
        }

        currentSavedFrame = currentSavedFrame->getParent();

        // Attach the asyncCause to the youngest frame.
        if (i == 0) {
            stackChain->back().asyncCause = asyncCauseAtom;
            firstSavedFrameParent = currentSavedFrame;
        }
    }

    // This is the 1-based index of the oldest frame we care about.
    size_t oldestFramePosition = stackChain->length();
    RootedSavedFrame parentFrame(cx, nullptr);

    if (currentSavedFrame == nullptr &&
        asyncStack->compartment() == cx->compartment()) {
        // If we consumed the full async stack, and the stack is in the same
        // compartment as the one requested, we don't need to rebuild the full
        // chain again using the lookup objects, we can just reference the
        // existing chain and change the asyncCause on the younger frame.
        oldestFramePosition = 1;
        parentFrame = firstSavedFrameParent;
    } else if (maxFrameCount == 0 &&
               oldestFramePosition == ASYNC_STACK_MAX_FRAME_COUNT) {
        // If we captured the maximum number of frames and the caller requested
        // no specific limit, we only return half of them. This means that for
        // the next iterations, it's likely we can use the optimization above.
        oldestFramePosition = ASYNC_STACK_MAX_FRAME_COUNT / 2;
    }

    // Iterate through |stackChain| in reverse order and get or create the
    // actual SavedFrame instances.
    for (size_t i = oldestFramePosition; i != 0; i--) {
        SavedFrame::HandleLookup lookup = stackChain[i-1];
        lookup->parent = parentFrame;
        parentFrame.set(getOrCreateSavedFrame(cx, lookup));
        if (!parentFrame)
            return false;
    }

    adoptedStack.set(parentFrame);
    return true;
}

SavedFrame*
SavedStacks::getOrCreateSavedFrame(JSContext* cx, SavedFrame::HandleLookup lookup)
{
    const SavedFrame::Lookup& lookupInstance = lookup.get();
    DependentAddPtr<SavedFrame::Set> p(cx, frames, lookupInstance);
    if (p)
        return *p;

    RootedSavedFrame frame(cx, createFrameFromLookup(cx, lookup));
    if (!frame)
        return nullptr;

    if (!p.add(cx, frames, lookupInstance, frame))
        return nullptr;

    return frame;
}

SavedFrame*
SavedStacks::createFrameFromLookup(JSContext* cx, SavedFrame::HandleLookup lookup)
{
    RootedSavedFrame frame(cx, SavedFrame::create(cx));
    if (!frame)
        return nullptr;
    frame->initFromLookup(lookup);

    if (!FreezeObject(cx, frame))
        return nullptr;

    return frame;
}

/*
 * Remove entries from the table whose JSScript is being collected.
 */
void
SavedStacks::sweepPCLocationMap()
{
    for (PCLocationMap::Enum e(pcLocationMap); !e.empty(); e.popFront()) {
        PCKey key = e.front().key();
        JSScript* script = key.script.get();
        if (IsAboutToBeFinalizedUnbarriered(&script)) {
            e.removeFront();
        } else if (script != key.script.get()) {
            key.script = script;
            e.rekeyFront(key);
        }
    }
}

bool
SavedStacks::getLocation(JSContext* cx, const FrameIter& iter, MutableHandleLocationValue locationp)
{
    // We should only ever be caching location values for scripts in this
    // compartment. Otherwise, we would get dead cross-compartment scripts in
    // the cache because our compartment's sweep method isn't called when their
    // compartment gets collected.
    assertSameCompartment(cx, this, iter.compartment());

    // When we have a |JSScript| for this frame, use a potentially memoized
    // location from our PCLocationMap and copy it into |locationp|. When we do
    // not have a |JSScript| for this frame (asm.js frames), we take a slow path
    // that doesn't employ memoization, and update |locationp|'s slots directly.

    if (!iter.hasScript()) {
        if (const char16_t* displayURL = iter.scriptDisplayURL()) {
            locationp->source = AtomizeChars(cx, displayURL, js_strlen(displayURL));
        } else {
            const char* filename = iter.scriptFilename() ? iter.scriptFilename() : "";
            locationp->source = Atomize(cx, filename, strlen(filename));
        }
        if (!locationp->source)
            return false;

        locationp->line = iter.computeLine(&locationp->column);
        // XXX: Make the column 1-based as in other browsers, instead of 0-based
        // which is how SpiderMonkey stores it internally. This will be
        // unnecessary once bug 1144340 is fixed.
        locationp->column++;
        return true;
    }

    RootedScript script(cx, iter.script());
    jsbytecode* pc = iter.pc();

    PCKey key(script, pc);
    PCLocationMap::AddPtr p = pcLocationMap.lookupForAdd(key);

    if (!p) {
        RootedAtom source(cx);
        if (const char16_t* displayURL = iter.scriptDisplayURL()) {
            source = AtomizeChars(cx, displayURL, js_strlen(displayURL));
        } else {
            const char* filename = script->filename() ? script->filename() : "";
            source = Atomize(cx, filename, strlen(filename));
        }
        if (!source)
            return false;

        uint32_t column;
        uint32_t line = PCToLineNumber(script, pc, &column);

        // Make the column 1-based. See comment above.
        LocationValue value(source, line, column + 1);
        if (!pcLocationMap.add(p, key, value)) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    locationp.set(p->value());
    return true;
}

void
SavedStacks::chooseSamplingProbability(JSCompartment* compartment)
{
    GlobalObject* global = compartment->maybeGlobal();
    if (!global)
        return;

    GlobalObject::DebuggerVector* dbgs = global->getDebuggers();
    if (!dbgs || dbgs->empty())
        return;

    mozilla::DebugOnly<Debugger**> begin = dbgs->begin();
    mozilla::DebugOnly<bool> foundAnyDebuggers = false;

    double probability = 0;
    for (Debugger** dbgp = dbgs->begin(); dbgp < dbgs->end(); dbgp++) {
        // The set of debuggers had better not change while we're iterating,
        // such that the vector gets reallocated.
        MOZ_ASSERT(dbgs->begin() == begin);

        if ((*dbgp)->trackingAllocationSites && (*dbgp)->enabled) {
            foundAnyDebuggers = true;
            probability = std::max((*dbgp)->allocationSamplingProbability,
                                   probability);
        }
    }
    MOZ_ASSERT(foundAnyDebuggers);

    bernoulli.setProbability(probability);
}

JSObject*
SavedStacksMetadataCallback(JSContext* cx, JSObject* target)
{
    RootedObject obj(cx, target);

    SavedStacks& stacks = cx->compartment()->savedStacks();
    if (!stacks.bernoulli.trial())
        return nullptr;

    AutoEnterOOMUnsafeRegion oomUnsafe;
    RootedSavedFrame frame(cx);
    if (!stacks.saveCurrentStack(cx, &frame))
        oomUnsafe.crash("SavedStacksMetadataCallback");

    if (!Debugger::onLogAllocationSite(cx, obj, frame, JS_GetCurrentEmbedderTime()))
        oomUnsafe.crash("SavedStacksMetadataCallback");

    MOZ_ASSERT_IF(frame, !frame->is<WrapperObject>());
    return frame;
}

#ifdef JS_CRASH_DIAGNOSTICS
void
CompartmentChecker::check(SavedStacks* stacks)
{
    if (&compartment->savedStacks() != stacks) {
        printf("*** Compartment SavedStacks mismatch: %p vs. %p\n",
               (void*) &compartment->savedStacks(), stacks);
        MOZ_CRASH();
    }
}
#endif /* JS_CRASH_DIAGNOSTICS */

/* static */ ReconstructedSavedFramePrincipals ReconstructedSavedFramePrincipals::IsSystem;
/* static */ ReconstructedSavedFramePrincipals ReconstructedSavedFramePrincipals::IsNotSystem;

} /* namespace js */

namespace JS {
namespace ubi {

bool
ConcreteStackFrame<SavedFrame>::isSystem() const
{
    auto trustedPrincipals = get().runtimeFromAnyThread()->trustedPrincipals();
    return get().getPrincipals() == trustedPrincipals ||
           get().getPrincipals() == &js::ReconstructedSavedFramePrincipals::IsSystem;
}

bool
ConcreteStackFrame<SavedFrame>::constructSavedFrameStack(JSContext* cx,
                                                         MutableHandleObject outSavedFrameStack)
    const
{
    outSavedFrameStack.set(&get());
    if (!cx->compartment()->wrap(cx, outSavedFrameStack)) {
        outSavedFrameStack.set(nullptr);
        return false;
    }
    return true;
}

// A `mozilla::Variant` matcher that converts the inner value of a
// `JS::ubi::AtomOrTwoByteChars` string to a `JSAtom*`.
struct MOZ_STACK_CLASS AtomizingMatcher
{
    using ReturnType = JSAtom*;

    JSContext* cx;
    size_t     length;

    explicit AtomizingMatcher(JSContext* cx, size_t length)
      : cx(cx)
      , length(length)
    { }

    JSAtom* match(JSAtom* atom) {
        MOZ_ASSERT(atom);
        return atom;
    }

    JSAtom* match(const char16_t* chars) {
        MOZ_ASSERT(chars);
        return AtomizeChars(cx, chars, length);
    }
};

bool ConstructSavedFrameStackSlow(JSContext* cx, JS::ubi::StackFrame& frame,
                                  MutableHandleObject outSavedFrameStack)
{
    SavedFrame::AutoLookupVector stackChain(cx);
    Rooted<JS::ubi::StackFrame> ubiFrame(cx, frame);

    while (ubiFrame.get()) {
        // Convert the source and functionDisplayName strings to atoms.

        js::RootedAtom source(cx);
        AtomizingMatcher atomizer(cx, ubiFrame.get().sourceLength());
        source = ubiFrame.get().source().match(atomizer);
        if (!source)
            return false;

        js::RootedAtom functionDisplayName(cx);
        auto nameLength = ubiFrame.get().functionDisplayNameLength();
        if (nameLength > 0) {
            AtomizingMatcher atomizer(cx, nameLength);
            functionDisplayName = ubiFrame.get().functionDisplayName().match(atomizer);
            if (!functionDisplayName)
                return false;
        }

        auto principals = js::ReconstructedSavedFramePrincipals::getSingleton(ubiFrame.get());

        if (!stackChain->emplaceBack(source, ubiFrame.get().line(), ubiFrame.get().column(),
                                     functionDisplayName, /* asyncCause */ nullptr,
                                     /* parent */ nullptr, principals))
        {
            ReportOutOfMemory(cx);
            return false;
        }

        ubiFrame = ubiFrame.get().parent();
    }

    js::RootedSavedFrame parentFrame(cx);
    for (size_t i = stackChain->length(); i != 0; i--) {
        SavedFrame::HandleLookup lookup = stackChain[i-1];
        lookup->parent = parentFrame;
        parentFrame = cx->compartment()->savedStacks().getOrCreateSavedFrame(cx, lookup);
        if (!parentFrame)
            return false;
    }

    outSavedFrameStack.set(parentFrame);
    return true;
}


} // namespace ubi
} // namespace JS

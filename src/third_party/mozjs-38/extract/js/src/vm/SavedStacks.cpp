/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SavedStacks.h"

#include "mozilla/Attributes.h"

#include <math.h>

#include "jsapi.h"
#include "jscompartment.h"
#include "jsfriendapi.h"
#include "jshashutil.h"
#include "jsmath.h"
#include "jsnum.h"
#include "jsscript.h"
#include "prmjtime.h"

#include "gc/Marking.h"
#include "gc/Rooting.h"
#include "js/Vector.h"
#include "vm/Debugger.h"
#include "vm/StringBuffer.h"

#include "jscntxtinlines.h"

#include "vm/NativeObject-inl.h"

using mozilla::AddToHash;
using mozilla::HashString;

namespace js {

struct SavedFrame::Lookup {
    Lookup(JSAtom* source, uint32_t line, uint32_t column, JSAtom* functionDisplayName,
           SavedFrame* parent, JSPrincipals* principals)
      : source(source),
        line(line),
        column(column),
        functionDisplayName(functionDisplayName),
        parent(parent),
        principals(principals)
    {
        MOZ_ASSERT(source);
    }

    JSAtom*      source;
    uint32_t     line;
    uint32_t     column;
    JSAtom*      functionDisplayName;
    SavedFrame*  parent;
    JSPrincipals* principals;

    void trace(JSTracer* trc) {
        gc::MarkStringUnbarriered(trc, &source, "SavedFrame::Lookup::source");
        if (functionDisplayName) {
            gc::MarkStringUnbarriered(trc, &functionDisplayName,
                                      "SavedFrame::Lookup::functionDisplayName");
        }
        if (parent) {
            gc::MarkObjectUnbarriered(trc, &parent,
                                      "SavedFrame::Lookup::parent");
        }
    }
};

class MOZ_STACK_CLASS SavedFrame::AutoLookupVector : public JS::CustomAutoRooter {
  public:
    explicit AutoLookupVector(JSContext* cx)
      : JS::CustomAutoRooter(cx),
        lookups(cx)
    { }

    typedef Vector<Lookup, 20> LookupVector;
    inline LookupVector* operator->() { return &lookups; }
    inline Lookup& operator[](size_t i) { return lookups[i]; }

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
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(SavedFrame::JSSLOT_COUNT) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_SavedFrame) |
    JSCLASS_IS_ANONYMOUS,
    nullptr,                    // addProperty
    nullptr,                    // delProperty
    nullptr,                    // getProperty
    nullptr,                    // setProperty
    nullptr,                    // enumerate
    nullptr,                    // resolve
    nullptr,                    // convert
    SavedFrame::finalize,       // finalize
    nullptr,                    // call
    nullptr,                    // hasInstance
    nullptr,                    // construct
    nullptr,                    // trace

    // ClassSpec
    {
        GenericCreateConstructor<SavedFrame::construct, 0, JSFunction::FinalizeKind>,
        GenericCreatePrototype,
        SavedFrame::staticFunctions,
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
    return v.toInt32();
}

uint32_t
SavedFrame::getColumn()
{
    const Value& v = getReservedSlot(JSSLOT_COLUMN);
    return v.toInt32();
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

SavedFrame*
SavedFrame::getParent()
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
SavedFrame::initFromLookup(SavedFrame::HandleLookup lookup)
{
    MOZ_ASSERT(lookup->source);
    MOZ_ASSERT(getReservedSlot(JSSLOT_SOURCE).isUndefined());
    setReservedSlot(JSSLOT_SOURCE, StringValue(lookup->source));

    setReservedSlot(JSSLOT_LINE, NumberValue(lookup->line));
    setReservedSlot(JSSLOT_COLUMN, NumberValue(lookup->column));
    setReservedSlot(JSSLOT_FUNCTIONDISPLAYNAME,
                    lookup->functionDisplayName
                        ? StringValue(lookup->functionDisplayName)
                        : NullValue());
    setReservedSlot(JSSLOT_PARENT, ObjectOrNullValue(lookup->parent));
    setReservedSlot(JSSLOT_PRIVATE_PARENT, PrivateValue(lookup->parent));

    MOZ_ASSERT(getReservedSlot(JSSLOT_PRINCIPALS).isUndefined());
    if (lookup->principals)
        JS_HoldPrincipals(lookup->principals);
    setReservedSlot(JSSLOT_PRINCIPALS, PrivateValue(lookup->principals));
}

bool
SavedFrame::parentMoved()
{
    const Value& v = getReservedSlot(JSSLOT_PRIVATE_PARENT);
    JSObject* p = static_cast<JSObject*>(v.toPrivate());
    return p == getParent();
}

void
SavedFrame::updatePrivateParent()
{
    setReservedSlot(JSSLOT_PRIVATE_PARENT, PrivateValue(getParent()));
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
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                         "SavedFrame");
    return false;
}

// Return the first SavedFrame in the chain that starts with |frame| whose
// principals are subsumed by |principals|, according to |subsumes|. If there is
// no such frame, return nullptr.
static SavedFrame*
GetFirstSubsumedFrame(JSContext* cx, HandleSavedFrame frame)
{
    JSSubsumesOp subsumes = cx->runtime()->securityCallbacks->subsumes;
    if (!subsumes)
        return frame;

    JSPrincipals* principals = cx->compartment()->principals;

    RootedSavedFrame rootedFrame(cx, frame);
    while (rootedFrame && !subsumes(principals, rootedFrame->getPrincipals()))
        rootedFrame = rootedFrame->getParent();

    return rootedFrame;
}

JS_FRIEND_API(JSObject*)
GetFirstSubsumedSavedFrame(JSContext* cx, HandleObject savedFrame)
{
    if (!savedFrame)
        return nullptr;
    RootedSavedFrame frame(cx, &savedFrame->as<SavedFrame>());
    return GetFirstSubsumedFrame(cx, frame);
}

/* static */ bool
SavedFrame::checkThis(JSContext* cx, CallArgs& args, const char* fnName,
                      MutableHandleSavedFrame frame)
{
    const Value& thisValue = args.thisv();

    if (!thisValue.isObject()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_NOT_NONNULL_OBJECT);
        return false;
    }

    JSObject* thisObject = CheckedUnwrap(&thisValue.toObject());
    if (!thisObject || !thisObject->is<SavedFrame>()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             SavedFrame::class_.name, fnName,
                             thisObject ? thisObject->getClass()->name : "object");
        return false;
    }

    // Check for SavedFrame.prototype, which has the same class as SavedFrame
    // instances, however doesn't actually represent a captured stack frame. It
    // is the only object that is<SavedFrame>() but doesn't have a source.
    if (thisObject->as<SavedFrame>().getReservedSlot(JSSLOT_SOURCE).isNull()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             SavedFrame::class_.name, fnName, "prototype object");
        return false;
    }

    // The caller might not have the principals to see this frame's data, so get
    // the first one they _do_ have access to.
    RootedSavedFrame rooted(cx, &thisObject->as<SavedFrame>());
    frame.set(GetFirstSubsumedFrame(cx, rooted));
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
//   - Value      defaultVal
// These parameters will be defined after calling this macro:
//   - CallArgs args
//   - Rooted<SavedFrame*> frame (will be non-null)
#define THIS_SAVEDFRAME(cx, argc, vp, fnName, defaultVal, args, frame) \
    CallArgs args = CallArgsFromVp(argc, vp);                          \
    RootedSavedFrame frame(cx);                                        \
    if (!checkThis(cx, args, fnName, &frame))                          \
        return false;                                                  \
    if (!frame) {                                                      \
        args.rval().set(defaultVal);                                   \
        return true;                                                   \
    }

/* static */ bool
SavedFrame::sourceProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get source)", NullValue(), args, frame);
    args.rval().setString(frame->getSource());
    return true;
}

/* static */ bool
SavedFrame::lineProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get line)", NullValue(), args, frame);
    uint32_t line = frame->getLine();
    args.rval().setNumber(line);
    return true;
}

/* static */ bool
SavedFrame::columnProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get column)", NullValue(), args, frame);
    uint32_t column = frame->getColumn();
    args.rval().setNumber(column);
    return true;
}

/* static */ bool
SavedFrame::functionDisplayNameProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get functionDisplayName)", NullValue(), args, frame);
    RootedAtom name(cx, frame->getFunctionDisplayName());
    if (name)
        args.rval().setString(name);
    else
        args.rval().setNull();
    return true;
}

/* static */ bool
SavedFrame::parentProperty(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "(get parent)", NullValue(), args, frame);
    RootedSavedFrame parent(cx, frame->getParent());
    args.rval().setObjectOrNull(GetFirstSubsumedFrame(cx, parent));
    return true;
}

/* static */ bool
SavedFrame::toStringMethod(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_SAVEDFRAME(cx, argc, vp, "toString", StringValue(cx->runtime()->emptyString), args, frame);
    StringBuffer sb(cx);
    DebugOnly<JSSubsumesOp> subsumes = cx->runtime()->securityCallbacks->subsumes;
    DebugOnly<JSPrincipals*> principals = cx->compartment()->principals;

    RootedSavedFrame parent(cx);
    do {
        MOZ_ASSERT_IF(subsumes, (*subsumes)(principals, frame->getPrincipals()));
        if (frame->isSelfHosted())
            goto nextIteration;

        {
            RootedAtom name(cx, frame->getFunctionDisplayName());
            if ((name && !sb.append(name))
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
        }

    nextIteration:
        parent = frame->getParent();
        frame = GetFirstSubsumedFrame(cx, parent);
    } while (frame);

    JSString* str = sb.finishString();
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

bool
SavedStacks::init()
{
    if (!pcLocationMap.init())
        return false;

    return frames.init();
}

bool
SavedStacks::saveCurrentStack(JSContext* cx, MutableHandleSavedFrame frame, unsigned maxFrameCount)
{
    MOZ_ASSERT(initialized());
    assertSameCompartment(cx, this);

    FrameIter iter(cx, FrameIter::ALL_CONTEXTS, FrameIter::GO_THROUGH_SAVED);
    return insertFrames(cx, iter, frame, maxFrameCount);
}

void
SavedStacks::sweep(JSRuntime* rt)
{
    if (frames.initialized()) {
        for (SavedFrame::Set::Enum e(frames); !e.empty(); e.popFront()) {
            JSObject* obj = e.front().unbarrieredGet();
            JSObject* temp = obj;

            if (IsObjectAboutToBeFinalizedFromAnyThread(&obj)) {
                e.removeFront();
            } else {
                SavedFrame* frame = &obj->as<SavedFrame>();
                bool parentMoved = frame->parentMoved();

                if (parentMoved) {
                    frame->updatePrivateParent();
                }

                if (obj != temp || parentMoved) {
                    e.rekeyFront(SavedFrame::Lookup(frame->getSource(),
                                                    frame->getLine(),
                                                    frame->getColumn(),
                                                    frame->getFunctionDisplayName(),
                                                    frame->getParent(),
                                                    frame->getPrincipals()),
                                 ReadBarriered<SavedFrame*>(frame));
                }
            }
        }
    }

    sweepPCLocationMap();
}

void
SavedStacks::trace(JSTracer* trc)
{
    if (!pcLocationMap.initialized())
        return;

    // Mark each of the source strings in our pc to location cache.
    for (PCLocationMap::Enum e(pcLocationMap); !e.empty(); e.popFront()) {
        LocationValue& loc = e.front().value();
        MarkString(trc, &loc.source, "SavedStacks::PCLocationMap's memoized script source name");
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

    // Accumulate the vector of Lookup objects in |stackChain|.
    SavedFrame::AutoLookupVector stackChain(cx);
    while (!iter.done()) {
        AutoLocationValueRooter location(cx);

        {
            AutoCompartment ac(cx, iter.compartment());
            if (!cx->compartment()->savedStacks().getLocation(cx, iter, &location))
                return false;
        }

        // Use growByUninitialized and placement-new instead of just append.
        // We'd ideally like to use an emplace method once Vector supports it.
        if (!stackChain->growByUninitialized(1))
            return false;
        new (&stackChain->back()) SavedFrame::Lookup(
          location->source,
          location->line,
          location->column,
          iter.isNonEvalFunctionFrame() ? iter.functionDisplayAtom() : nullptr,
          nullptr,
          iter.compartment()->principals
        );

        ++iter;

        if (maxFrameCount == 0) {
            // If maxFrameCount is zero, then there's no limit on the number of
            // frames.
            continue;
        } else if (maxFrameCount == 1) {
            // Since we were only asked to save one frame, do not continue
            // walking the stack and saving frame state.
            break;
        } else {
            maxFrameCount--;
        }
    }

    // Iterate through |stackChain| in reverse order and get or create the
    // actual SavedFrame instances.
    RootedSavedFrame parentFrame(cx, nullptr);
    for (size_t i = stackChain->length(); i != 0; i--) {
        SavedFrame::AutoLookupRooter lookup(cx, &stackChain[i-1]);
        lookup->parent = parentFrame;
        parentFrame.set(getOrCreateSavedFrame(cx, lookup));
        if (!parentFrame)
            return false;
    }

    frame.set(parentFrame);
    return true;
}

SavedFrame*
SavedStacks::getOrCreateSavedFrame(JSContext* cx, SavedFrame::HandleLookup lookup)
{
    const SavedFrame::Lookup& lookupInstance = *lookup;
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
    RootedGlobalObject global(cx, cx->global());
    assertSameCompartment(cx, global);

    RootedNativeObject proto(cx, GlobalObject::getOrCreateSavedFramePrototype(cx, global));
    if (!proto)
        return nullptr;
    assertSameCompartment(cx, proto);

    RootedObject frameObj(cx, NewObjectWithGivenProto(cx, &SavedFrame::class_, proto, global));
    if (!frameObj)
        return nullptr;

    RootedSavedFrame f(cx, &frameObj->as<SavedFrame>());
    f->initFromLookup(lookup);

    if (!FreezeObject(cx, frameObj))
        return nullptr;

    return f.get();
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
        if (IsScriptAboutToBeFinalizedFromAnyThread(&script)) {
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

        LocationValue value(source, line, column);
        if (!pcLocationMap.add(p, key, value))
            return false;
    }

    locationp.set(p->value());
    return true;
}

void
SavedStacks::chooseSamplingProbability(JSContext* cx)
{
    GlobalObject::DebuggerVector* dbgs = cx->global()->getDebuggers();
    if (!dbgs || dbgs->empty())
        return;

    Debugger* allocationTrackingDbg = nullptr;
    mozilla::DebugOnly<Debugger**> begin = dbgs->begin();

    for (Debugger** dbgp = dbgs->begin(); dbgp < dbgs->end(); dbgp++) {
        // The set of debuggers had better not change while we're iterating,
        // such that the vector gets reallocated.
        MOZ_ASSERT(dbgs->begin() == begin);

        if ((*dbgp)->trackingAllocationSites && (*dbgp)->enabled)
            allocationTrackingDbg = *dbgp;
    }

    if (!allocationTrackingDbg)
        return;

    allocationSamplingProbability = allocationTrackingDbg->allocationSamplingProbability;
}

bool
SavedStacksMetadataCallback(JSContext* cx, JSObject** pmetadata)
{
    SavedStacks& stacks = cx->compartment()->savedStacks();
    if (stacks.allocationSkipCount > 0) {
        stacks.allocationSkipCount--;
        return true;
    }

    stacks.chooseSamplingProbability(cx);
    if (stacks.allocationSamplingProbability == 0.0)
        return true;

    // If the sampling probability is set to 1.0, we are always taking a sample
    // and can therefore leave allocationSkipCount at 0.
    if (stacks.allocationSamplingProbability != 1.0) {
        // Rather than generating a random number on every allocation to decide
        // if we want to sample that particular allocation (which would be
        // expensive), we calculate the number of allocations to skip before
        // taking the next sample.
        //
        // P = the probability we sample any given event.
        //
        // ~P = 1-P, the probability we don't sample a given event.
        //
        // (~P)^n = the probability that we skip at least the next n events.
        //
        // let X = random between 0 and 1.
        //
        // floor(log base ~P of X) = n, aka the number of events we should skip
        // until we take the next sample. Any value for X less than (~P)^n
        // yields a skip count greater than n, so the likelihood of a skip count
        // greater than n is (~P)^n, as required.
        double notSamplingProb = 1.0 - stacks.allocationSamplingProbability;
        stacks.allocationSkipCount = std::floor(std::log(random_nextDouble(&stacks.rngState)) /
                                                std::log(notSamplingProb));
    }

    RootedSavedFrame frame(cx);
    if (!stacks.saveCurrentStack(cx, &frame))
        return false;
    *pmetadata = frame;

    return Debugger::onLogAllocationSite(cx, frame, PRMJ_Now());
}

JS_FRIEND_API(JSPrincipals*)
GetSavedFramePrincipals(HandleObject savedFrame)
{
    MOZ_ASSERT(savedFrame);
    MOZ_ASSERT(savedFrame->is<SavedFrame>());
    return savedFrame->as<SavedFrame>().getPrincipals();
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

} /* namespace js */

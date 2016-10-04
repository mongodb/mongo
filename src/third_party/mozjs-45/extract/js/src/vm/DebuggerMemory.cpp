/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/DebuggerMemory.h"

#include "mozilla/Maybe.h"
#include "mozilla/Move.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"

#include <stdlib.h>

#include "jsalloc.h"
#include "jscntxt.h"
#include "jscompartment.h"

#include "builtin/MapObject.h"
#include "gc/Marking.h"
#include "js/Debug.h"
#include "js/TracingAPI.h"
#include "js/UbiNode.h"
#include "js/UbiNodeCensus.h"
#include "js/Utility.h"
#include "vm/Debugger.h"
#include "vm/GlobalObject.h"
#include "vm/SavedStacks.h"

#include "vm/Debugger-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using JS::ubi::BreadthFirst;
using JS::ubi::Edge;
using JS::ubi::Node;

using mozilla::Forward;
using mozilla::Maybe;
using mozilla::Move;
using mozilla::Nothing;
using mozilla::UniquePtr;

/* static */ DebuggerMemory*
DebuggerMemory::create(JSContext* cx, Debugger* dbg)
{
    Value memoryProtoValue = dbg->object->getReservedSlot(Debugger::JSSLOT_DEBUG_MEMORY_PROTO);
    RootedObject memoryProto(cx, &memoryProtoValue.toObject());
    RootedNativeObject memory(cx, NewNativeObjectWithGivenProto(cx, &class_, memoryProto));
    if (!memory)
        return nullptr;

    dbg->object->setReservedSlot(Debugger::JSSLOT_DEBUG_MEMORY_INSTANCE, ObjectValue(*memory));
    memory->setReservedSlot(JSSLOT_DEBUGGER, ObjectValue(*dbg->object));

    return &memory->as<DebuggerMemory>();
}

Debugger*
DebuggerMemory::getDebugger()
{
    const Value& dbgVal = getReservedSlot(JSSLOT_DEBUGGER);
    return Debugger::fromJSObject(&dbgVal.toObject());
}

/* static */ bool
DebuggerMemory::construct(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                         "Debugger.Memory");
    return false;
}

/* static */ const Class DebuggerMemory::class_ = {
    "Memory",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_COUNT)
};

/* static */ DebuggerMemory*
DebuggerMemory::checkThis(JSContext* cx, CallArgs& args, const char* fnName)
{
    const Value& thisValue = args.thisv();

    if (!thisValue.isObject()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_NONNULL_OBJECT, InformalValueTypeName(thisValue));
        return nullptr;
    }

    JSObject& thisObject = thisValue.toObject();
    if (!thisObject.is<DebuggerMemory>()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             class_.name, fnName, thisObject.getClass()->name);
        return nullptr;
    }

    // Check for Debugger.Memory.prototype, which has the same class as
    // Debugger.Memory instances, however doesn't actually represent an instance
    // of Debugger.Memory. It is the only object that is<DebuggerMemory>() but
    // doesn't have a Debugger instance.
    if (thisObject.as<DebuggerMemory>().getReservedSlot(JSSLOT_DEBUGGER).isUndefined()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             class_.name, fnName, "prototype object");
        return nullptr;
    }

    return &thisObject.as<DebuggerMemory>();
}

/**
 * Get the |DebuggerMemory*| from the current this value and handle any errors
 * that might occur therein.
 *
 * These parameters must already exist when calling this macro:
 * - JSContext* cx
 * - unsigned argc
 * - Value* vp
 * - const char* fnName
 * These parameters will be defined after calling this macro:
 * - CallArgs args
 * - DebuggerMemory* memory (will be non-null)
 */
#define THIS_DEBUGGER_MEMORY(cx, argc, vp, fnName, args, memory)        \
    CallArgs args = CallArgsFromVp(argc, vp);                           \
    Rooted<DebuggerMemory*> memory(cx, checkThis(cx, args, fnName));    \
    if (!memory)                                                        \
        return false

static bool
undefined(CallArgs& args)
{
    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerMemory::setTrackingAllocationSites(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(set trackingAllocationSites)", args, memory);
    if (!args.requireAtLeast(cx, "(set trackingAllocationSites)", 1))
        return false;

    Debugger* dbg = memory->getDebugger();
    bool enabling = ToBoolean(args[0]);

    if (enabling == dbg->trackingAllocationSites)
        return undefined(args);

    dbg->trackingAllocationSites = enabling;

    if (!dbg->enabled)
        return undefined(args);

    if (enabling) {
        if (!dbg->addAllocationsTrackingForAllDebuggees(cx)) {
            dbg->trackingAllocationSites = false;
            return false;
        }
    } else {
        dbg->removeAllocationsTrackingForAllDebuggees();
    }

    return undefined(args);
}

/* static */ bool
DebuggerMemory::getTrackingAllocationSites(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get trackingAllocationSites)", args, memory);
    args.rval().setBoolean(memory->getDebugger()->trackingAllocationSites);
    return true;
}

/* static */ bool
DebuggerMemory::drainAllocationsLog(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "drainAllocationsLog", args, memory);
    Debugger* dbg = memory->getDebugger();

    if (!dbg->trackingAllocationSites) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_TRACKING_ALLOCATIONS,
                             "drainAllocationsLog");
        return false;
    }

    size_t length = dbg->allocationsLog.length();

    RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!result)
        return false;
    result->ensureDenseInitializedLength(cx, 0, length);

    for (size_t i = 0; i < length; i++) {
        RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!obj)
            return false;

        // Don't pop the AllocationsLogEntry yet. The queue's links are followed
        // by the GC to find the AllocationsLogEntry, but are not barriered, so
        // we must edit them with great care. Use the queue entry in place, and
        // then pop and delete together.
        Debugger::AllocationsLogEntry& entry = dbg->allocationsLog.front();

        RootedValue frame(cx, ObjectOrNullValue(entry.frame));
        if (!DefineProperty(cx, obj, cx->names().frame, frame))
            return false;

        RootedValue timestampValue(cx, NumberValue(entry.when));
        if (!DefineProperty(cx, obj, cx->names().timestamp, timestampValue))
            return false;

        RootedString className(cx, Atomize(cx, entry.className, strlen(entry.className)));
        if (!className)
            return false;
        RootedValue classNameValue(cx, StringValue(className));
        if (!DefineProperty(cx, obj, cx->names().class_, classNameValue))
            return false;

        RootedValue ctorName(cx, NullValue());
        if (entry.ctorName)
            ctorName.setString(entry.ctorName);
        if (!DefineProperty(cx, obj, cx->names().constructor, ctorName))
            return false;

        RootedValue size(cx, NumberValue(entry.size));
        if (!DefineProperty(cx, obj, cx->names().size, size))
            return false;

        RootedValue inNursery(cx, BooleanValue(entry.inNursery));
        if (!DefineProperty(cx, obj, cx->names().inNursery, inNursery))
            return false;

        result->setDenseElement(i, ObjectValue(*obj));

        // Pop the front queue entry, and delete it immediately, so that the GC
        // sees the AllocationsLogEntry's RelocatablePtr barriers run atomically
        // with the change to the graph (the queeue link).
        if (!dbg->allocationsLog.popFront()) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    dbg->allocationsLogOverflowed = false;
    args.rval().setObject(*result);
    return true;
}

/* static */ bool
DebuggerMemory::getMaxAllocationsLogLength(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get maxAllocationsLogLength)", args, memory);
    args.rval().setInt32(memory->getDebugger()->maxAllocationsLogLength);
    return true;
}

/* static */ bool
DebuggerMemory::setMaxAllocationsLogLength(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(set maxAllocationsLogLength)", args, memory);
    if (!args.requireAtLeast(cx, "(set maxAllocationsLogLength)", 1))
        return false;

    int32_t max;
    if (!ToInt32(cx, args[0], &max))
        return false;

    if (max < 1) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                             "(set maxAllocationsLogLength)'s parameter",
                             "not a positive integer");
        return false;
    }

    Debugger* dbg = memory->getDebugger();
    dbg->maxAllocationsLogLength = max;

    while (dbg->allocationsLog.length() > dbg->maxAllocationsLogLength) {
        if (!dbg->allocationsLog.popFront()) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerMemory::getAllocationSamplingProbability(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get allocationSamplingProbability)", args, memory);
    args.rval().setDouble(memory->getDebugger()->allocationSamplingProbability);
    return true;
}

/* static */ bool
DebuggerMemory::setAllocationSamplingProbability(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(set allocationSamplingProbability)", args, memory);
    if (!args.requireAtLeast(cx, "(set allocationSamplingProbability)", 1))
        return false;

    double probability;
    if (!ToNumber(cx, args[0], &probability))
        return false;

    // Careful!  This must also reject NaN.
    if (!(0.0 <= probability && probability <= 1.0)) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                             "(set allocationSamplingProbability)'s parameter",
                             "not a number between 0 and 1");
        return false;
    }

    Debugger* dbg = memory->getDebugger();
    if (dbg->allocationSamplingProbability != probability) {
        dbg->allocationSamplingProbability = probability;

        // If this is a change any debuggees would observe, have all debuggee
        // compartments recompute their sampling probabilities.
        if (dbg->enabled && dbg->trackingAllocationSites) {
            for (auto r = dbg->debuggees.all(); !r.empty(); r.popFront())
                r.front()->compartment()->chooseAllocationSamplingProbability();
        }
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerMemory::getAllocationsLogOverflowed(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get allocationsLogOverflowed)", args, memory);
    args.rval().setBoolean(memory->getDebugger()->allocationsLogOverflowed);
    return true;
}

/* static */ bool
DebuggerMemory::setTrackingTenurePromotions(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(set trackingTenurePromotions)", args, memory);
    if (!args.requireAtLeast(cx, "(set trackingTenurePromotions)", 1))
        return false;

    Debugger* dbg = memory->getDebugger();
    dbg->trackingTenurePromotions = ToBoolean(args[0]);
    return undefined(args);
}

/* static */ bool
DebuggerMemory::getTrackingTenurePromotions(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get trackingTenurePromotions)", args, memory);
    args.rval().setBoolean(memory->getDebugger()->trackingTenurePromotions);
    return true;
}

/* static */ bool
DebuggerMemory::drainTenurePromotionsLog(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "drainTenurePromotionsLog", args, memory);
    Debugger* dbg = memory->getDebugger();

    if (!dbg->trackingTenurePromotions) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_TRACKING_TENURINGS,
                             "drainTenurePromotionsLog");
        return false;
    }

    size_t length = dbg->tenurePromotionsLog.length();

    RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!result)
        return false;
    result->ensureDenseInitializedLength(cx, 0, length);

    for (size_t i = 0; i < length; i++) {
        RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!obj)
            return false;

        // Don't pop the TenurePromotionsEntry yet. The queue's links are
        // followed by the GC to find the TenurePromotionsEntry, but are not
        // barriered, so we must edit them with great care. Use the queue entry
        // in place, and then pop and delete together.
        auto& entry = dbg->tenurePromotionsLog.front();

        RootedValue frame(cx, ObjectOrNullValue(entry.frame));
        if (!cx->compartment()->wrap(cx, &frame) ||
            !DefineProperty(cx, obj, cx->names().frame, frame))
        {
            return false;
        }

        RootedValue timestampValue(cx, NumberValue(entry.when));
        if (!DefineProperty(cx, obj, cx->names().timestamp, timestampValue))
            return false;

        RootedString className(cx, Atomize(cx, entry.className, strlen(entry.className)));
        if (!className)
            return false;
        RootedValue classNameValue(cx, StringValue(className));
        if (!DefineProperty(cx, obj, cx->names().class_, classNameValue))
            return false;

        RootedValue sizeValue(cx, NumberValue(entry.size));
        if (!DefineProperty(cx, obj, cx->names().size, sizeValue))
            return false;

        result->setDenseElement(i, ObjectValue(*obj));

        // Pop the front queue entry, and delete it immediately, so that the GC
        // sees the TenurePromotionsEntry's RelocatablePtr barriers run
        // atomically with the change to the graph (the queue link).
        if (!dbg->tenurePromotionsLog.popFront()) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    dbg->tenurePromotionsLogOverflowed = false;
    args.rval().setObject(*result);
    return true;
}

/* static */ bool
DebuggerMemory::getMaxTenurePromotionsLogLength(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get maxTenurePromotionsLogLength)", args, memory);
    args.rval().setInt32(memory->getDebugger()->maxTenurePromotionsLogLength);
    return true;
}

/* static */ bool
DebuggerMemory::setMaxTenurePromotionsLogLength(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(set maxTenurePromotionsLogLength)", args, memory);
    if (!args.requireAtLeast(cx, "(set maxTenurePromotionsLogLength)", 1))
        return false;

    int32_t max;
    if (!ToInt32(cx, args[0], &max))
        return false;

    if (max < 1) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                             "(set maxTenurePromotionsLogLength)'s parameter",
                             "not a positive integer");
        return false;
    }

    Debugger* dbg = memory->getDebugger();
    dbg->maxTenurePromotionsLogLength = max;

    while (dbg->tenurePromotionsLog.length() > dbg->maxAllocationsLogLength) {
        if (!dbg->tenurePromotionsLog.popFront()) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerMemory::getTenurePromotionsLogOverflowed(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get tenurePromotionsLogOverflowed)", args, memory);
    args.rval().setBoolean(memory->getDebugger()->tenurePromotionsLogOverflowed);
    return true;
}

/* static */ bool
DebuggerMemory::getOnGarbageCollection(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get onGarbageCollection)", args, memory);
    return Debugger::getHookImpl(cx, args, *memory->getDebugger(), Debugger::OnGarbageCollection);
}

/* static */ bool
DebuggerMemory::setOnGarbageCollection(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(set onGarbageCollection)", args, memory);
    return Debugger::setHookImpl(cx, args, *memory->getDebugger(), Debugger::OnGarbageCollection);
}


/* Debugger.Memory.prototype.takeCensus */

JS_PUBLIC_API(void)
JS::dbg::SetDebuggerMallocSizeOf(JSRuntime* rt, mozilla::MallocSizeOf mallocSizeOf)
{
    rt->debuggerMallocSizeOf = mallocSizeOf;
}

JS_PUBLIC_API(mozilla::MallocSizeOf)
JS::dbg::GetDebuggerMallocSizeOf(JSRuntime* rt)
{
    return rt->debuggerMallocSizeOf;
}

using JS::ubi::Census;
using JS::ubi::CountTypePtr;
using JS::ubi::CountBasePtr;

// The takeCensus function works in three phases:
//
// 1) We examine the 'breakdown' property of our 'options' argument, and
//    use that to build a CountType tree.
//
// 2) We create a count node for the root of our CountType tree, and then walk
//    the heap, counting each node we find, expanding our tree of counts as we
//    go.
//
// 3) We walk the tree of counts and produce JavaScript objects reporting the
//    accumulated results.
bool
DebuggerMemory::takeCensus(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "Debugger.Memory.prototype.census", args, memory);

    Census census(cx);
    if (!census.init())
        return false;
    CountTypePtr rootType;

    RootedObject options(cx);
    if (args.get(0).isObject())
        options = &args[0].toObject();

    if (!JS::ubi::ParseCensusOptions(cx, census, options, rootType))
        return false;

    JS::ubi::RootedCount rootCount(cx, rootType->makeCount());
    if (!rootCount)
        return false;
    JS::ubi::CensusHandler handler(census, rootCount);

    Debugger* dbg = memory->getDebugger();
    RootedObject dbgObj(cx, dbg->object);

    // Populate our target set of debuggee zones.
    for (WeakGlobalObjectSet::Range r = dbg->allDebuggees(); !r.empty(); r.popFront()) {
        if (!census.targetZones.put(r.front()->zone()))
            return false;
    }

    {
        Maybe<JS::AutoCheckCannotGC> maybeNoGC;
        JS::ubi::RootList rootList(cx->runtime(), maybeNoGC);
        if (!rootList.init(dbgObj)) {
            ReportOutOfMemory(cx);
            return false;
        }

        JS::ubi::CensusTraversal traversal(cx->runtime(), handler, maybeNoGC.ref());
        if (!traversal.init()) {
            ReportOutOfMemory(cx);
            return false;
        }
        traversal.wantNames = false;

        if (!traversal.addStart(JS::ubi::Node(&rootList)) ||
            !traversal.traverse())
        {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    return handler.report(args.rval());
}


/* Debugger.Memory property and method tables. */


/* static */ const JSPropertySpec DebuggerMemory::properties[] = {
    JS_PSGS("trackingAllocationSites", getTrackingAllocationSites, setTrackingAllocationSites, 0),
    JS_PSGS("maxAllocationsLogLength", getMaxAllocationsLogLength, setMaxAllocationsLogLength, 0),
    JS_PSGS("allocationSamplingProbability", getAllocationSamplingProbability, setAllocationSamplingProbability, 0),
    JS_PSG("allocationsLogOverflowed", getAllocationsLogOverflowed, 0),

    JS_PSGS("trackingTenurePromotions", getTrackingTenurePromotions, setTrackingTenurePromotions, 0),
    JS_PSGS("maxTenurePromotionsLogLength", getMaxTenurePromotionsLogLength, setMaxTenurePromotionsLogLength, 0),
    JS_PSG("tenurePromotionsLogOverflowed", getTenurePromotionsLogOverflowed, 0),

    JS_PSGS("onGarbageCollection", getOnGarbageCollection, setOnGarbageCollection, 0),
    JS_PS_END
};

/* static */ const JSFunctionSpec DebuggerMemory::methods[] = {
    JS_FN("drainAllocationsLog", DebuggerMemory::drainAllocationsLog, 0, 0),
    JS_FN("drainTenurePromotionsLog", DebuggerMemory::drainTenurePromotionsLog, 0, 0),
    JS_FN("takeCensus", takeCensus, 0, 0),
    JS_FS_END
};

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "debugger/DebuggerMemory.h"

#include "mozilla/Maybe.h"
#include "mozilla/Vector.h"

#include <stdlib.h>
#include <utility>

#include "jsapi.h"

#include "builtin/MapObject.h"
#include "debugger/Debugger.h"
#include "gc/Marking.h"
#include "js/AllocPolicy.h"
#include "js/Debug.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/TracingAPI.h"
#include "js/UbiNode.h"
#include "js/UbiNodeCensus.h"
#include "js/Utility.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Realm.h"
#include "vm/SavedStacks.h"

#include "debugger/Debugger-inl.h"
#include "gc/StableCellHasher-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::Maybe;
using mozilla::Nothing;

/* static */
DebuggerMemory* DebuggerMemory::create(JSContext* cx, Debugger* dbg) {
  Value memoryProtoValue =
      dbg->object->getReservedSlot(Debugger::JSSLOT_DEBUG_MEMORY_PROTO);
  RootedObject memoryProto(cx, &memoryProtoValue.toObject());
  Rooted<DebuggerMemory*> memory(
      cx, NewObjectWithGivenProto<DebuggerMemory>(cx, memoryProto));
  if (!memory) {
    return nullptr;
  }

  dbg->object->setReservedSlot(Debugger::JSSLOT_DEBUG_MEMORY_INSTANCE,
                               ObjectValue(*memory));
  memory->setReservedSlot(JSSLOT_DEBUGGER, ObjectValue(*dbg->object));

  return memory;
}

Debugger* DebuggerMemory::getDebugger() {
  const Value& dbgVal = getReservedSlot(JSSLOT_DEBUGGER);
  return Debugger::fromJSObject(&dbgVal.toObject());
}

/* static */
bool DebuggerMemory::construct(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                            "Debugger.Source");
  return false;
}

/* static */ const JSClass DebuggerMemory::class_ = {
    "Memory", JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_COUNT)};

/* static */
DebuggerMemory* DebuggerMemory::checkThis(JSContext* cx, CallArgs& args) {
  const Value& thisValue = args.thisv();

  if (!thisValue.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED,
                              InformalValueTypeName(thisValue));
    return nullptr;
  }

  JSObject& thisObject = thisValue.toObject();
  if (!thisObject.is<DebuggerMemory>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, class_.name, "method",
                              thisObject.getClass()->name);
    return nullptr;
  }

  return &thisObject.as<DebuggerMemory>();
}

struct MOZ_STACK_CLASS DebuggerMemory::CallData {
  JSContext* cx;
  const CallArgs& args;

  Handle<DebuggerMemory*> memory;

  CallData(JSContext* cx, const CallArgs& args, Handle<DebuggerMemory*> memory)
      : cx(cx), args(args), memory(memory) {}

  // Accessor properties of Debugger.Memory.prototype.

  bool setTrackingAllocationSites();
  bool getTrackingAllocationSites();
  bool setMaxAllocationsLogLength();
  bool getMaxAllocationsLogLength();
  bool setAllocationSamplingProbability();
  bool getAllocationSamplingProbability();
  bool getAllocationsLogOverflowed();
  bool getOnGarbageCollection();
  bool setOnGarbageCollection();

  // Function properties of Debugger.Memory.prototype.

  bool takeCensus();
  bool drainAllocationsLog();

  using Method = bool (CallData::*)();

  template <Method MyMethod>
  static bool ToNative(JSContext* cx, unsigned argc, Value* vp);
};

template <DebuggerMemory::CallData::Method MyMethod>
/* static */
bool DebuggerMemory::CallData::ToNative(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DebuggerMemory*> memory(cx, DebuggerMemory::checkThis(cx, args));
  if (!memory) {
    return false;
  }

  CallData data(cx, args, memory);
  return (data.*MyMethod)();
}

static bool undefined(const CallArgs& args) {
  args.rval().setUndefined();
  return true;
}

bool DebuggerMemory::CallData::setTrackingAllocationSites() {
  if (!args.requireAtLeast(cx, "(set trackingAllocationSites)", 1)) {
    return false;
  }

  Debugger* dbg = memory->getDebugger();
  bool enabling = ToBoolean(args[0]);

  if (enabling == dbg->trackingAllocationSites) {
    return undefined(args);
  }

  dbg->trackingAllocationSites = enabling;

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

bool DebuggerMemory::CallData::getTrackingAllocationSites() {
  args.rval().setBoolean(memory->getDebugger()->trackingAllocationSites);
  return true;
}

bool DebuggerMemory::CallData::drainAllocationsLog() {
  Debugger* dbg = memory->getDebugger();

  if (!dbg->trackingAllocationSites) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_TRACKING_ALLOCATIONS,
                              "drainAllocationsLog");
    return false;
  }

  size_t length = dbg->allocationsLog.length();

  Rooted<ArrayObject*> result(cx, NewDenseFullyAllocatedArray(cx, length));
  if (!result) {
    return false;
  }
  result->ensureDenseInitializedLength(0, length);

  for (size_t i = 0; i < length; i++) {
    Rooted<PlainObject*> obj(cx, NewPlainObject(cx));
    if (!obj) {
      return false;
    }

    // Don't pop the AllocationsLogEntry yet. The queue's links are followed
    // by the GC to find the AllocationsLogEntry, but are not barriered, so
    // we must edit them with great care. Use the queue entry in place, and
    // then pop and delete together.
    Debugger::AllocationsLogEntry& entry = dbg->allocationsLog.front();

    RootedValue frame(cx, ObjectOrNullValue(entry.frame));
    if (!DefineDataProperty(cx, obj, cx->names().frame, frame)) {
      return false;
    }

    double when =
        (entry.when - mozilla::TimeStamp::ProcessCreation()).ToMilliseconds();
    RootedValue timestampValue(cx, NumberValue(when));
    if (!DefineDataProperty(cx, obj, cx->names().timestamp, timestampValue)) {
      return false;
    }

    RootedString className(
        cx, Atomize(cx, entry.className, strlen(entry.className)));
    if (!className) {
      return false;
    }
    RootedValue classNameValue(cx, StringValue(className));
    if (!DefineDataProperty(cx, obj, cx->names().class_, classNameValue)) {
      return false;
    }

    RootedValue size(cx, NumberValue(entry.size));
    if (!DefineDataProperty(cx, obj, cx->names().size, size)) {
      return false;
    }

    RootedValue inNursery(cx, BooleanValue(entry.inNursery));
    if (!DefineDataProperty(cx, obj, cx->names().inNursery, inNursery)) {
      return false;
    }

    result->setDenseElement(i, ObjectValue(*obj));

    // Pop the front queue entry, and delete it immediately, so that the GC
    // sees the AllocationsLogEntry's HeapPtr barriers run atomically with
    // the change to the graph (the queue link).
    dbg->allocationsLog.popFront();
  }

  dbg->allocationsLogOverflowed = false;
  args.rval().setObject(*result);
  return true;
}

bool DebuggerMemory::CallData::getMaxAllocationsLogLength() {
  args.rval().setInt32(memory->getDebugger()->maxAllocationsLogLength);
  return true;
}

bool DebuggerMemory::CallData::setMaxAllocationsLogLength() {
  if (!args.requireAtLeast(cx, "(set maxAllocationsLogLength)", 1)) {
    return false;
  }

  int32_t max;
  if (!ToInt32(cx, args[0], &max)) {
    return false;
  }

  if (max < 1) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
        "(set maxAllocationsLogLength)'s parameter", "not a positive integer");
    return false;
  }

  Debugger* dbg = memory->getDebugger();
  dbg->maxAllocationsLogLength = max;

  while (dbg->allocationsLog.length() > dbg->maxAllocationsLogLength) {
    dbg->allocationsLog.popFront();
  }

  args.rval().setUndefined();
  return true;
}

bool DebuggerMemory::CallData::getAllocationSamplingProbability() {
  args.rval().setDouble(memory->getDebugger()->allocationSamplingProbability);
  return true;
}

bool DebuggerMemory::CallData::setAllocationSamplingProbability() {
  if (!args.requireAtLeast(cx, "(set allocationSamplingProbability)", 1)) {
    return false;
  }

  double probability;
  if (!ToNumber(cx, args[0], &probability)) {
    return false;
  }

  // Careful!  This must also reject NaN.
  if (!(0.0 <= probability && probability <= 1.0)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_UNEXPECTED_TYPE,
                              "(set allocationSamplingProbability)'s parameter",
                              "not a number between 0 and 1");
    return false;
  }

  Debugger* dbg = memory->getDebugger();
  if (dbg->allocationSamplingProbability != probability) {
    dbg->allocationSamplingProbability = probability;

    // If this is a change any debuggees would observe, have all debuggee
    // realms recompute their sampling probabilities.
    if (dbg->trackingAllocationSites) {
      for (auto r = dbg->debuggees.all(); !r.empty(); r.popFront()) {
        r.front()->realm()->chooseAllocationSamplingProbability();
      }
    }
  }

  args.rval().setUndefined();
  return true;
}

bool DebuggerMemory::CallData::getAllocationsLogOverflowed() {
  args.rval().setBoolean(memory->getDebugger()->allocationsLogOverflowed);
  return true;
}

bool DebuggerMemory::CallData::getOnGarbageCollection() {
  return Debugger::getGarbageCollectionHook(cx, args, *memory->getDebugger());
}

bool DebuggerMemory::CallData::setOnGarbageCollection() {
  return Debugger::setGarbageCollectionHook(cx, args, *memory->getDebugger());
}

/* Debugger.Memory.prototype.takeCensus */

JS_PUBLIC_API void JS::dbg::SetDebuggerMallocSizeOf(
    JSContext* cx, mozilla::MallocSizeOf mallocSizeOf) {
  cx->runtime()->debuggerMallocSizeOf = mallocSizeOf;
}

JS_PUBLIC_API mozilla::MallocSizeOf JS::dbg::GetDebuggerMallocSizeOf(
    JSContext* cx) {
  return cx->runtime()->debuggerMallocSizeOf;
}

using JS::ubi::Census;
using JS::ubi::CountBasePtr;
using JS::ubi::CountTypePtr;

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
bool DebuggerMemory::CallData::takeCensus() {
  Census census(cx);
  CountTypePtr rootType;

  RootedObject options(cx);
  if (args.get(0).isObject()) {
    options = &args[0].toObject();
  }

  if (!JS::ubi::ParseCensusOptions(cx, census, options, rootType)) {
    return false;
  }

  JS::ubi::RootedCount rootCount(cx, rootType->makeCount());
  if (!rootCount) {
    ReportOutOfMemory(cx);
    return false;
  }
  JS::ubi::CensusHandler handler(census, rootCount,
                                 cx->runtime()->debuggerMallocSizeOf);

  Debugger* dbg = memory->getDebugger();
  RootedObject dbgObj(cx, dbg->object);

  // Populate our target set of debuggee zones.
  for (WeakGlobalObjectSet::Range r = dbg->allDebuggees(); !r.empty();
       r.popFront()) {
    if (!census.targetZones.put(r.front()->zone())) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  {
    JS::ubi::RootList rootList(cx);
    auto [ok, nogc] = rootList.init(dbgObj);
    if (!ok) {
      ReportOutOfMemory(cx);
      return false;
    }

    JS::ubi::CensusTraversal traversal(cx, handler, nogc);
    traversal.wantNames = false;

    if (!traversal.addStart(JS::ubi::Node(&rootList)) ||
        !traversal.traverse()) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  return handler.report(cx, args.rval());
}

/* Debugger.Memory property and method tables. */

/* static */ const JSPropertySpec DebuggerMemory::properties[] = {
    JS_DEBUG_PSGS("trackingAllocationSites", getTrackingAllocationSites,
                  setTrackingAllocationSites),
    JS_DEBUG_PSGS("maxAllocationsLogLength", getMaxAllocationsLogLength,
                  setMaxAllocationsLogLength),
    JS_DEBUG_PSGS("allocationSamplingProbability",
                  getAllocationSamplingProbability,
                  setAllocationSamplingProbability),
    JS_DEBUG_PSG("allocationsLogOverflowed", getAllocationsLogOverflowed),
    JS_DEBUG_PSGS("onGarbageCollection", getOnGarbageCollection,
                  setOnGarbageCollection),
    JS_PS_END};

/* static */ const JSFunctionSpec DebuggerMemory::methods[] = {
    JS_DEBUG_FN("drainAllocationsLog", drainAllocationsLog, 0),
    JS_DEBUG_FN("takeCensus", takeCensus, 0), JS_FS_END};

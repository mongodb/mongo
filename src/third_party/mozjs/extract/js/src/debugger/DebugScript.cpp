/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "debugger/DebugScript.h"

#include "mozilla/Assertions.h"  // for AssertionConditionType
#include "mozilla/HashTable.h"   // for HashMapEntry, HashTable<>::Ptr, HashMap
#include "mozilla/UniquePtr.h"   // for UniquePtr

#include <utility>  // for std::move

#include "debugger/DebugAPI.h"    // for DebugAPI
#include "debugger/Debugger.h"    // for JSBreakpointSite, Breakpoint
#include "gc/Cell.h"              // for TenuredCell
#include "gc/GCContext.h"         // for JS::GCContext
#include "gc/GCEnum.h"            // for MemoryUse, MemoryUse::BreakpointSite
#include "gc/Marking.h"           // for IsAboutToBeFinalized
#include "gc/Zone.h"              // for Zone
#include "gc/ZoneAllocator.h"     // for AddCellMemory
#include "jit/BaselineJIT.h"      // for BaselineScript
#include "vm/BytecodeIterator.h"  // for AllBytecodesIterable
#include "vm/JSContext.h"         // for JSContext
#include "vm/JSScript.h"          // for JSScript, DebugScriptMap
#include "vm/NativeObject.h"      // for NativeObject
#include "vm/Realm.h"             // for Realm, AutoRealm
#include "vm/Runtime.h"           // for ReportOutOfMemory
#include "vm/Stack.h"             // for ActivationIterator, Activation

#include "gc/GC-inl.h"                // for ZoneCellIter
#include "gc/GCContext-inl.h"         // for JS::GCContext::free_
#include "gc/Marking-inl.h"           // for CheckGCThingAfterMovingGC
#include "gc/WeakMap-inl.h"           // for WeakMap::remove
#include "vm/BytecodeIterator-inl.h"  // for AllBytecodesIterable
#include "vm/JSContext-inl.h"         // for JSContext::check
#include "vm/JSObject-inl.h"          // for NewObjectWithGivenProto
#include "vm/JSScript-inl.h"          // for JSScript::hasBaselineScript
#include "vm/Realm-inl.h"             // for AutoRealm::AutoRealm

namespace js {

const JSClass DebugScriptObject::class_ = {
    "DebugScriptObject",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) | JSCLASS_BACKGROUND_FINALIZE,
    &classOps_, JS_NULL_CLASS_SPEC};

const JSClassOps DebugScriptObject::classOps_ = {
    nullptr,                      // addProperty
    nullptr,                      // delProperty
    nullptr,                      // enumerate
    nullptr,                      // newEnumerate
    nullptr,                      // resolve
    nullptr,                      // mayResolve
    DebugScriptObject::finalize,  // finalize
    nullptr,                      // call
    nullptr,                      // construct
    DebugScriptObject::trace,     // trace
};

/* static */
DebugScriptObject* DebugScriptObject::create(JSContext* cx,
                                             UniqueDebugScript debugScript,
                                             size_t nbytes) {
  auto* object = NewObjectWithGivenProto<DebugScriptObject>(cx, nullptr);
  if (!object) {
    return nullptr;
  }

  object->initReservedSlot(ScriptSlot, PrivateValue(debugScript.release()));
  AddCellMemory(object, nbytes, MemoryUse::ScriptDebugScript);

  return object;
}

DebugScript* DebugScriptObject::debugScript() const {
  return maybePtrFromReservedSlot<DebugScript>(ScriptSlot);
}

/* static */
void DebugScriptObject::trace(JSTracer* trc, JSObject* obj) {
  DebugScript* debugScript = obj->as<DebugScriptObject>().debugScript();
  if (debugScript) {
    debugScript->trace(trc);
  }
}

/* static */
void DebugScriptObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  DebugScriptObject* object = &obj->as<DebugScriptObject>();
  DebugScript* debugScript = object->debugScript();
  if (debugScript) {
    debugScript->delete_(gcx, object);
  }
}

/* static */
DebugScript* DebugScript::get(JSScript* script) {
  MOZ_ASSERT(script->hasDebugScript());
  DebugScriptMap* map = script->zone()->debugScriptMap;
  MOZ_ASSERT(map);
  DebugScriptMap::Ptr p = map->lookupUnbarriered(script);
  MOZ_ASSERT(p);
  return p->value().get()->as<DebugScriptObject>().debugScript();
}

/* static */
DebugScript* DebugScript::getOrCreate(JSContext* cx, HandleScript script) {
  cx->check(script);

  if (script->hasDebugScript()) {
    return get(script);
  }

  size_t nbytes = allocSize(script->length());
  UniqueDebugScript debug(
      reinterpret_cast<DebugScript*>(cx->pod_calloc<uint8_t>(nbytes)));
  if (!debug) {
    return nullptr;
  }

  debug->codeLength = script->length();

  Rooted<DebugScriptObject*> object(
      cx, DebugScriptObject::create(cx, std::move(debug), nbytes));
  if (!object) {
    return nullptr;
  }

  /* Create zone's debugScriptMap if necessary. */
  Zone* zone = script->zone();
  MOZ_ASSERT(cx->zone() == zone);
  if (!zone->debugScriptMap) {
    DebugScriptMap* map = cx->new_<DebugScriptMap>(cx);
    if (!map) {
      return nullptr;
    }

    zone->debugScriptMap = map;
  }

  MOZ_ASSERT(script->hasBytecode());

  if (!zone->debugScriptMap->putNew(script.get(), object.get())) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  // It is safe to set this: we can't fail after this point.
  script->setHasDebugScript(true);

  /*
   * Ensure that any Interpret() instances running on this script have
   * interrupts enabled. The interrupts must stay enabled until the
   * debug state is destroyed.
   */
  for (ActivationIterator iter(cx); !iter.done(); ++iter) {
    if (iter->isInterpreter()) {
      iter->asInterpreter()->enableInterruptsIfRunning(script);
    }
  }

  return object->debugScript();
}

/* static */
JSBreakpointSite* DebugScript::getBreakpointSite(JSScript* script,
                                                 jsbytecode* pc) {
  uint32_t offset = script->pcToOffset(pc);
  return script->hasDebugScript() ? get(script)->breakpoints[offset] : nullptr;
}

/* static */
JSBreakpointSite* DebugScript::getOrCreateBreakpointSite(JSContext* cx,
                                                         HandleScript script,
                                                         jsbytecode* pc) {
  AutoRealm ar(cx, script);

  DebugScript* debug = getOrCreate(cx, script);
  if (!debug) {
    return nullptr;
  }

  JSBreakpointSite*& site = debug->breakpoints[script->pcToOffset(pc)];

  if (!site) {
    site = cx->new_<JSBreakpointSite>(script, pc);
    if (!site) {
      return nullptr;
    }
    debug->numSites++;
    AddCellMemory(script, sizeof(JSBreakpointSite), MemoryUse::BreakpointSite);

    if (script->hasBaselineScript()) {
      script->baselineScript()->toggleDebugTraps(script, pc);
    }
  }

  return site;
}

/* static */
void DebugScript::destroyBreakpointSite(JS::GCContext* gcx, JSScript* script,
                                        jsbytecode* pc) {
  DebugScript* debug = get(script);
  JSBreakpointSite*& site = debug->breakpoints[script->pcToOffset(pc)];
  MOZ_ASSERT(site);
  MOZ_ASSERT(site->isEmpty());

  site->delete_(gcx);
  site = nullptr;

  debug->numSites--;
  if (!debug->needed()) {
    DebugAPI::removeDebugScript(gcx, script);
  }

  if (script->hasBaselineScript()) {
    script->baselineScript()->toggleDebugTraps(script, pc);
  }
}

/* static */
void DebugScript::clearBreakpointsIn(JS::GCContext* gcx, JSScript* script,
                                     Debugger* dbg, JSObject* handler) {
  MOZ_ASSERT(script);
  // Breakpoints hold wrappers in the script's compartment for the handler. Make
  // sure we don't try to search for the unwrapped handler.
  MOZ_ASSERT_IF(handler, script->compartment() == handler->compartment());

  if (!script->hasDebugScript()) {
    return;
  }

  AllBytecodesIterable iter(script);
  for (BytecodeLocation loc : iter) {
    JSBreakpointSite* site = getBreakpointSite(script, loc.toRawBytecode());
    if (site) {
      Breakpoint* nextbp;
      for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = nextbp) {
        nextbp = bp->nextInSite();
        if ((!dbg || bp->debugger == dbg) &&
            (!handler || bp->getHandler() == handler)) {
          bp->remove(gcx);
        }
      }
    }
  }
}

#ifdef DEBUG
/* static */
uint32_t DebugScript::getStepperCount(JSScript* script) {
  return script->hasDebugScript() ? get(script)->stepperCount : 0;
}
#endif  // DEBUG

/* static */
bool DebugScript::incrementStepperCount(JSContext* cx, HandleScript script) {
  cx->check(script);
  MOZ_ASSERT(cx->realm()->isDebuggee());

  AutoRealm ar(cx, script);

  DebugScript* debug = getOrCreate(cx, script);
  if (!debug) {
    return false;
  }

  debug->stepperCount++;

  if (debug->stepperCount == 1) {
    if (script->hasBaselineScript()) {
      script->baselineScript()->toggleDebugTraps(script, nullptr);
    }
  }

  return true;
}

/* static */
void DebugScript::decrementStepperCount(JS::GCContext* gcx, JSScript* script) {
  DebugScript* debug = get(script);
  MOZ_ASSERT(debug);
  MOZ_ASSERT(debug->stepperCount > 0);

  debug->stepperCount--;

  if (debug->stepperCount == 0) {
    if (script->hasBaselineScript()) {
      script->baselineScript()->toggleDebugTraps(script, nullptr);
    }

    if (!debug->needed()) {
      DebugAPI::removeDebugScript(gcx, script);
    }
  }
}

/* static */
bool DebugScript::incrementGeneratorObserverCount(JSContext* cx,
                                                  HandleScript script) {
  cx->check(script);
  MOZ_ASSERT(cx->realm()->isDebuggee());

  AutoRealm ar(cx, script);

  DebugScript* debug = getOrCreate(cx, script);
  if (!debug) {
    return false;
  }

  debug->generatorObserverCount++;

  // It is our caller's responsibility, before bumping the generator observer
  // count, to make sure that the baseline code includes the necessary
  // JSOp::AfterYield instrumentation by calling
  // {ensure,update}ExecutionObservabilityOfScript.
  MOZ_ASSERT_IF(script->hasBaselineScript(),
                script->baselineScript()->hasDebugInstrumentation());

  return true;
}

/* static */
void DebugScript::decrementGeneratorObserverCount(JS::GCContext* gcx,
                                                  JSScript* script) {
  DebugScript* debug = get(script);
  MOZ_ASSERT(debug);
  MOZ_ASSERT(debug->generatorObserverCount > 0);

  debug->generatorObserverCount--;

  if (!debug->needed()) {
    DebugAPI::removeDebugScript(gcx, script);
  }
}

void DebugScript::trace(JSTracer* trc) {
  for (size_t i = 0; i < codeLength; i++) {
    JSBreakpointSite* site = breakpoints[i];
    if (site) {
      site->trace(trc);
    }
  }
}

/* static */
void DebugAPI::removeDebugScript(JS::GCContext* gcx, JSScript* script) {
  if (script->hasDebugScript()) {
    if (IsAboutToBeFinalizedUnbarriered(script)) {
      // The script is dying and all breakpoint data will be cleaned up.
      return;
    }

    DebugScriptMap* map = script->zone()->debugScriptMap;
    MOZ_ASSERT(map);
    DebugScriptMap::Ptr p = map->lookupUnbarriered(script);
    MOZ_ASSERT(p);
    map->remove(p);
    script->setHasDebugScript(false);

    // The DebugScript will be destroyed at the next GC when its owning
    // DebugScriptObject dies.
  }
}

void DebugScript::delete_(JS::GCContext* gcx, DebugScriptObject* owner) {
  for (size_t i = 0; i < codeLength; i++) {
    JSBreakpointSite* site = breakpoints[i];
    if (site) {
      site->delete_(gcx);
    }
  }

  gcx->free_(owner, this, allocSize(codeLength), MemoryUse::ScriptDebugScript);
}

#ifdef JSGC_HASH_TABLE_CHECKS
/* static */
void DebugAPI::checkDebugScriptAfterMovingGC(DebugScript* ds) {
  for (uint32_t i = 0; i < ds->numSites; i++) {
    JSBreakpointSite* site = ds->breakpoints[i];
    if (site) {
      CheckGCThingAfterMovingGC(site->script.get());
    }
  }
}
#endif  // JSGC_HASH_TABLE_CHECKS

/* static */
bool DebugAPI::stepModeEnabledSlow(JSScript* script) {
  return DebugScript::get(script)->stepperCount > 0;
}

/* static */
bool DebugAPI::hasBreakpointsAtSlow(JSScript* script, jsbytecode* pc) {
  JSBreakpointSite* site = DebugScript::getBreakpointSite(script, pc);
  return !!site;
}

/* static */
void DebugAPI::traceDebugScriptMap(JSTracer* trc, DebugScriptMap* map) {
  map->trace(trc);
}

/* static */
void DebugAPI::deleteDebugScriptMap(DebugScriptMap* map) { js_delete(map); }

}  // namespace js

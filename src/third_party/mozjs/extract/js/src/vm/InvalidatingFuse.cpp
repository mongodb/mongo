/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/InvalidatingFuse.h"

#include "gc/PublicIterators.h"
#include "jit/Invalidation.h"
#include "jit/JitSpewer.h"
#include "vm/JSContext.h"
#include "vm/JSScript.h"
#include "vm/Logging.h"

#include "gc/StableCellHasher-inl.h"
#include "vm/JSScript-inl.h"

js::DependentScriptSet::DependentScriptSet(JSContext* cx,
                                           InvalidatingFuse* fuse)
    : associatedFuse(fuse), weakScripts(cx->runtime()) {}

bool js::InvalidatingRuntimeFuse::addFuseDependency(JSContext* cx,
                                                    Handle<JSScript*> script) {
  auto* zone = script->zone();
  DependentScriptSet* dss =
      zone->fuseDependencies.getOrCreateDependentScriptSet(cx, this);
  if (!dss) {
    return false;
  }

  return dss->addScriptForFuse(this, script);
}

void js::InvalidatingRuntimeFuse::popFuse(JSContext* cx) {
  // Pop the fuse in the base class
  GuardFuse::popFuse(cx);
  JS_LOG(fuseInvalidation, Verbose, "Invalidating fuse popping: %s", name());
  // do invalidation.
  for (AllZonesIter z(cx->runtime()); !z.done(); z.next()) {
    // There's one dependent script set per fuse; just iterate over them all to
    // find the one we need (see comment on JS::Zone::fuseDependencies for
    // reasoning).
    for (auto& fd : z.get()->fuseDependencies) {
      fd.invalidateForFuse(cx, this);
    }
  }
}

void js::DependentScriptSet::invalidateForFuse(JSContext* cx,
                                               InvalidatingFuse* fuse) {
  if (associatedFuse != fuse) {
    return;
  }

  jit::InvalidateAndClearScriptSet(cx, weakScripts, "fuse");
}

void js::jit::InvalidateAndClearScriptSet(JSContext* cx,
                                          WeakScriptCache& scripts,
                                          const char* reason) {
  // Move the cache contents into this local -- this clears the other one, and
  // also protects from js::jit::Invalidate trying to modify scripts out from
  // under us. See ClearPendingInvalidationDependencies.
  WeakScriptSet localScripts = scripts.stealContents();
  MOZ_ASSERT(scripts.empty());

  for (auto r = localScripts.all(); !r.empty(); r.popFront()) {
    JSScript* script = r.front().get();
    // A script may have lost its ion script for other reasons
    // by the time this is invoked, so need to ensure it's still there
    // before calling invalidate.
    if (script->hasIonScript()) {
      JitSpew(jit::JitSpew_IonInvalidate, "Invalidating ion script %p for %s",
              script->ionScript(), reason);
      JS_LOG(fuseInvalidation, Debug,
             "Invalidating ion script %s:%d for reason %s", script->filename(),
             script->lineno(), reason);
      js::jit::Invalidate(cx, script);
    }
  }
}

bool js::DependentScriptSet::addScriptForFuse(InvalidatingFuse* fuse,
                                              Handle<JSScript*> script) {
  MOZ_ASSERT(fuse == associatedFuse);
  return jit::AddScriptToSet(weakScripts, script);
}

js::DependentScriptSet* js::DependentScriptGroup::getOrCreateDependentScriptSet(
    JSContext* cx, js::InvalidatingFuse* fuse) {
  for (auto& dss : dependencies) {
    if (dss.associatedFuse == fuse) {
      return &dss;
    }
  }

  if (!dependencies.emplaceBack(cx, fuse)) {
    return nullptr;
  }

  auto& dss = dependencies.back();
  MOZ_ASSERT(dss.associatedFuse == fuse);
  return &dss;
}

bool js::jit::AddScriptToSet(WeakScriptCache& scripts,
                             Handle<JSScript*> script) {
  js::jit::WeakScriptSet::AddPtr p = scripts.lookupForAdd(script);
  if (!p) {
    if (!scripts.add(p, script)) {
      return false;
    }
  }

  // Script is already in the set, no need to re-add.
  return true;
}

void js::jit::RemoveFromScriptSet(WeakScriptCache& scripts, JSScript* script) {
  js::jit::WeakScriptSet::Ptr p = scripts.lookup(script);
  if (p) {
    scripts.remove(p);
  }
}

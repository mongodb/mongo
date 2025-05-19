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

  for (auto r = weakScripts.all(); !r.empty(); r.popFront()) {
    JSScript* script = r.front().get();
    // A script may have lost its ion script for other reasons
    // by the time this is invoked, so need to ensure it's still there
    // before calling invaidate.
    if (script->hasIonScript()) {
      JitSpew(jit::JitSpew_IonInvalidate, "Invalidating ion script %p",
              script->ionScript());
      js::jit::Invalidate(cx, script);
    }
  }

  // Scripts are invalidated, flush them.
  weakScripts.clear();
}

bool js::DependentScriptSet::addScriptForFuse(InvalidatingFuse* fuse,
                                              Handle<JSScript*> script) {
  MOZ_ASSERT(fuse == associatedFuse);

  WeakScriptSet::AddPtr p = weakScripts.lookupForAdd(script);
  if (!p) {
    if (!weakScripts.add(p, script)) {
      return false;
    }
  }

  // Script is already in the set, no need to re-add.
  return true;
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

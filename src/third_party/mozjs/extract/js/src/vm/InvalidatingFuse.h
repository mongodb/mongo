/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_InvalidatingFuse_h
#define vm_InvalidatingFuse_h

#include "gc/Barrier.h"
#include "jit/InvalidationScriptSet.h"
#include "js/SweepingAPI.h"

#include "vm/GuardFuse.h"
class JSScript;

namespace js {

// [SMDOC] Invalidating Fuses
//
// An invalidating fuse will invalidate a set of dependent IonScripts when the
// fuse is popped. In this way Ion can choose to ignore fuse guarded
// possibilities when doing compilation.
class InvalidatingFuse : public GuardFuse {
 public:
  // Register a script's IonScript as having a dependency on this fuse.
  virtual bool addFuseDependency(JSContext* cx, Handle<JSScript*> script) = 0;
};

// [SMDOC] Invalidating Runtime Fuses
//
// A specialized sublass for handling runtime wide fuses. This provides a
// version of addFuseDependency which records scripts into sets associated with
// their home zone, and invalidates all sets across all zones linked to this
// specific fuse.
class InvalidatingRuntimeFuse : public InvalidatingFuse {
 public:
  virtual bool addFuseDependency(JSContext* cx,
                                 Handle<JSScript*> script) override;
  virtual void popFuse(JSContext* cx) override;
};

// A (weak) set of scripts which are dependent on an associated fuse.
//
// Because it uses JS::WeakCache, GC tracing is taken care of without any need
// for tracing in this class.
class DependentScriptSet {
 public:
  DependentScriptSet(JSContext* cx, InvalidatingFuse* fuse);

  InvalidatingFuse* associatedFuse;
  bool addScriptForFuse(InvalidatingFuse* fuse, Handle<JSScript*> script);
  void invalidateForFuse(JSContext* cx, InvalidatingFuse* fuse);

  void removeScript(JSScript* script) {
    jit::RemoveFromScriptSet(weakScripts, script);
  }

 private:
  js::jit::WeakScriptCache weakScripts;
};

class DependentScriptGroup {
  // A dependent script set pairs a fuse with a set of scripts which depend
  // on said fuse; this is a vector of script sets because the expectation for
  // now is that the number of runtime wide invalidating fuses will be small.
  // This will need to be revisited (convert to HashMap?) should that no
  // longer be the case
  //
  // Note: This isn't  traced through the zone, but rather through the use
  // of JS::WeakCache.
  Vector<DependentScriptSet, 1, SystemAllocPolicy> dependencies;

 public:
  DependentScriptSet* getOrCreateDependentScriptSet(JSContext* cx,
                                                    InvalidatingFuse* fuse);
  DependentScriptSet* begin() { return dependencies.begin(); }
  DependentScriptSet* end() { return dependencies.end(); }

  void removeScript(JSScript* script) {
    for (auto& set : dependencies) {
      set.removeScript(script);
    }
  }
};

}  // namespace js

#endif  // vm_InvalidatingFuse_h

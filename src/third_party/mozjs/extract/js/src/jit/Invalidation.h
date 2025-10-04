/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Invalidation_h
#define jit_Invalidation_h

#include "jit/IonTypes.h"
#include "js/AllocPolicy.h"
#include "js/GCVector.h"

namespace js {
namespace jit {

class IonScript;

class RecompileInfo {
  JSScript* script_;
  IonCompilationId id_;

 public:
  RecompileInfo(JSScript* script, IonCompilationId id)
      : script_(script), id_(id) {}

  JSScript* script() const { return script_; }

  IonScript* maybeIonScriptToInvalidate() const;

  bool traceWeak(JSTracer* trc);

  bool operator==(const RecompileInfo& other) const {
    return script_ == other.script_ && id_ == other.id_;
  }
};

// The RecompileInfoVector has a MinInlineCapacity of one so that invalidating a
// single IonScript doesn't require an allocation.
using RecompileInfoVector = JS::GCVector<RecompileInfo, 1, SystemAllocPolicy>;

// Called from Zone::discardJitCode().
void InvalidateAll(JS::GCContext* gcx, JS::Zone* zone);
void FinishInvalidation(JS::GCContext* gcx, JSScript* script);

// Add compilations involving |script| (outer script or inlined) to the vector.
void AddPendingInvalidation(jit::RecompileInfoVector& invalid,
                            JSScript* script);

// Walk the stack and invalidate active Ion frames for the invalid scripts.
void Invalidate(JSContext* cx, const RecompileInfoVector& invalid,
                bool resetUses = true, bool cancelOffThread = true);
void Invalidate(JSContext* cx, JSScript* script, bool resetUses = true,
                bool cancelOffThread = true);

}  // namespace jit
}  // namespace js

#endif /* jit_Invalidation_h */

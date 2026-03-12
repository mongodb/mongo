/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_WarpOracle_h
#define jit_WarpOracle_h

#include "jit/JitAllocPolicy.h"
#include "jit/JitContext.h"
#include "jit/WarpSnapshot.h"

namespace js {
namespace jit {

class MIRGenerator;

// WarpOracle creates a WarpSnapshot data structure that's used by WarpBuilder
// to generate the MIR graph off-thread.
class MOZ_STACK_CLASS WarpOracle {
  JSContext* cx_;
  MIRGenerator& mirGen_;
  TempAllocator& alloc_;
  HandleScript outerScript_;
  WarpBailoutInfo bailoutInfo_;
  WarpScriptSnapshotList scriptSnapshots_;
  WarpZoneStubsSnapshot zoneStubs_{};
  size_t accumulatedBytecodeSize_ = 0;
#ifdef DEBUG
  mozilla::HashNumber runningScriptHash_ = 0;
#endif

  // List of nursery objects to copy to the snapshot. See WarpObjectField.
  // The HashMap is used to de-duplicate the Vector. It maps each object to the
  // corresponding nursery index (index into the Vector).
  // Note: this stores raw object pointers because WarpOracle can't GC.
  Vector<JSObject*, 8, SystemAllocPolicy> nurseryObjects_;
  using NurseryObjectsMap =
      HashMap<JSObject*, uint32_t, DefaultHasher<JSObject*>, SystemAllocPolicy>;
  NurseryObjectsMap nurseryObjectsMap_;

 public:
  WarpOracle(JSContext* cx, MIRGenerator& mirGen, HandleScript outerScript);
  ~WarpOracle() { scriptSnapshots_.clear(); }

  MIRGenerator& mirGen() { return mirGen_; }
  WarpBailoutInfo& bailoutInfo() { return bailoutInfo_; }

  [[nodiscard]] bool registerNurseryObject(JSObject* obj,
                                           uint32_t* nurseryIndex);

  [[nodiscard]] bool snapshotJitZoneStub(JitZone::StubKind kind);

  AbortReasonOr<WarpSnapshot*> createSnapshot();

  mozilla::GenericErrorResult<AbortReason> abort(HandleScript script,
                                                 AbortReason r);
  mozilla::GenericErrorResult<AbortReason> abort(HandleScript script,
                                                 AbortReason r,
                                                 const char* message, ...);
  void addScriptSnapshot(WarpScriptSnapshot* scriptSnapshot, ICScript* icScript,
                         size_t bytecodeLength);

  size_t accumulatedBytecodeSize() { return accumulatedBytecodeSize_; }
  void ignoreFailedICHash();
};

}  // namespace jit
}  // namespace js

#endif /* jit_WarpOracle_h */

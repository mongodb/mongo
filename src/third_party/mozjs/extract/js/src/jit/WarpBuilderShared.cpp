/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/WarpBuilderShared.h"

#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace js::jit;

WarpBuilderShared::WarpBuilderShared(WarpSnapshot& snapshot,
                                     MIRGenerator& mirGen,
                                     MBasicBlock* current_)
    : snapshot_(snapshot),
      mirGen_(mirGen),
      alloc_(mirGen.alloc()),
      current(current_) {}

bool WarpBuilderShared::resumeAfter(MInstruction* ins, BytecodeLocation loc) {
  // resumeAfter should only be used with effectful instructions. The only
  // exceptions are:
  // 1. MInt64ToBigInt, which is used to convert the result of either a call
  //    into Wasm code or loading from a BigIntArray, so we attach the resume
  //    point to that instead of to the call resp. load.
  // 2. MPostIntPtrConversion which is used after conversion from IntPtr.
  MOZ_ASSERT(ins->isEffectful() || ins->isInt64ToBigInt() ||
             ins->isPostIntPtrConversion());
  MOZ_ASSERT(!ins->isMovable());

  MResumePoint* resumePoint = MResumePoint::New(
      alloc(), ins->block(), loc.toRawBytecode(), ResumeMode::ResumeAfter);
  if (!resumePoint) {
    return false;
  }

  ins->setResumePoint(resumePoint);
  return true;
}

MConstant* WarpBuilderShared::constant(const Value& v) {
  MOZ_ASSERT_IF(v.isString(), v.toString()->isLinear());
  MOZ_ASSERT_IF(v.isGCThing(), !IsInsideNursery(v.toGCThing()));

  MConstant* cst = MConstant::New(alloc(), v);
  current->add(cst);
  return cst;
}

void WarpBuilderShared::pushConstant(const Value& v) {
  MConstant* cst = constant(v);
  current->push(cst);
}

MDefinition* WarpBuilderShared::unboxObjectInfallible(MDefinition* def,
                                                      IsMovable movable) {
  if (def->type() == MIRType::Object) {
    return def;
  }

  if (def->type() != MIRType::Value) {
    // Corner case: if the MIR node has a type other than Object or Value, this
    // code isn't actually reachable and we expect an earlier guard to fail.
    // Just insert a Box to satisfy MIR invariants.
    MOZ_ASSERT(movable == IsMovable::No);
    auto* box = MBox::New(alloc(), def);
    current->add(box);
    def = box;
  }

  auto* unbox = MUnbox::New(alloc(), def, MIRType::Object, MUnbox::Infallible);
  if (movable == IsMovable::No) {
    unbox->setNotMovable();
  }
  current->add(unbox);
  return unbox;
}

MCall* WarpBuilderShared::makeCall(CallInfo& callInfo, bool needsThisCheck,
                                   WrappedFunction* target, bool isDOMCall,
                                   gc::Heap initialHeap) {
  auto addUndefined = [this]() -> MConstant* {
    return constant(UndefinedValue());
  };

  return MakeCall(alloc(), addUndefined, callInfo, needsThisCheck, target,
                  isDOMCall, initialHeap);
}

MInstruction* WarpBuilderShared::makeSpreadCall(CallInfo& callInfo,
                                                bool needsThisCheck,
                                                bool isSameRealm,
                                                WrappedFunction* target) {
  MOZ_ASSERT(callInfo.argFormat() == CallInfo::ArgFormat::Array);
  MOZ_ASSERT_IF(needsThisCheck, !target);

  // Load dense elements of the argument array.
  MElements* elements = MElements::New(alloc(), callInfo.arrayArg());
  current->add(elements);

  if (callInfo.constructing()) {
    auto* newTarget = unboxObjectInfallible(callInfo.getNewTarget());
    auto* construct =
        MConstructArray::New(alloc(), target, callInfo.callee(), elements,
                             callInfo.thisArg(), newTarget);
    if (isSameRealm) {
      construct->setNotCrossRealm();
    }
    if (needsThisCheck) {
      construct->setNeedsThisCheck();
    }
    return construct;
  }

  auto* apply = MApplyArray::New(alloc(), target, callInfo.callee(), elements,
                                 callInfo.thisArg());

  if (callInfo.ignoresReturnValue()) {
    apply->setIgnoresReturnValue();
  }
  if (isSameRealm) {
    apply->setNotCrossRealm();
  }
  MOZ_ASSERT(!needsThisCheck);
  return apply;
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_WarpBuilderShared_h
#define jit_WarpBuilderShared_h

#include "mozilla/Attributes.h"

#include "jit/MIRGraph.h"
#include "js/Value.h"

namespace js {

class BytecodeLocation;

namespace jit {

class MBasicBlock;
class MCall;
class MConstant;
class MInstruction;
class MIRGenerator;
class TempAllocator;
class WarpSnapshot;
class WrappedFunction;

// Helper class to manage call state.
class MOZ_STACK_CLASS CallInfo {
  MDefinition* callee_ = nullptr;
  MDefinition* thisArg_ = nullptr;
  MDefinition* newTargetArg_ = nullptr;
  MDefinitionVector args_;

  bool constructing_;

  // True if the caller does not use the return value.
  bool ignoresReturnValue_;

  bool inlined_ = false;
  bool setter_ = false;

 public:
  // For normal calls and FunCall we can shuffle around definitions in
  // the CallInfo and use a normal MCall. For others, we need to use a
  // specialized call.
  enum class ArgFormat {
    Standard,
    Array,
    FunApplyArgsObj,
  };

 private:
  ArgFormat argFormat_ = ArgFormat::Standard;

 public:
  CallInfo(TempAllocator& alloc, bool constructing, bool ignoresReturnValue,
           jsbytecode* pc = nullptr)
      : args_(alloc),
        constructing_(constructing),
        ignoresReturnValue_(ignoresReturnValue) {}

  [[nodiscard]] bool init(MBasicBlock* current, uint32_t argc) {
    MOZ_ASSERT(args_.empty());

    // Get the arguments in the right order
    if (!args_.reserve(argc)) {
      return false;
    }

    if (constructing()) {
      setNewTarget(current->pop());
    }

    for (int32_t i = argc; i > 0; i--) {
      args_.infallibleAppend(current->peek(-i));
    }
    current->popn(argc);

    // Get |this| and |callee|
    setThis(current->pop());
    setCallee(current->pop());

    return true;
  }

  void initForSpreadCall(MBasicBlock* current) {
    MOZ_ASSERT(args_.empty());

    if (constructing()) {
      setNewTarget(current->pop());
    }

    // Spread calls have one argument, an Array object containing the args.
    static_assert(decltype(args_)::InlineLength >= 1,
                  "Appending one argument should be infallible");
    MOZ_ALWAYS_TRUE(args_.append(current->pop()));

    // Get |this| and |callee|
    setThis(current->pop());
    setCallee(current->pop());

    argFormat_ = ArgFormat::Array;
  }

  void initForGetterCall(MDefinition* callee, MDefinition* thisVal) {
    MOZ_ASSERT(args_.empty());
    setCallee(callee);
    setThis(thisVal);
  }
  void initForSetterCall(MDefinition* callee, MDefinition* thisVal,
                         MDefinition* rhs) {
    MOZ_ASSERT(args_.empty());
    markAsSetter();
    setCallee(callee);
    setThis(thisVal);
    static_assert(decltype(args_)::InlineLength >= 1,
                  "Appending one argument should be infallible");
    MOZ_ALWAYS_TRUE(args_.append(rhs));
  }

  void initForApplyInlinedArgs(MDefinition* callee, MDefinition* thisVal,
                               uint32_t numActuals) {
    MOZ_ASSERT(args_.empty());
    MOZ_ASSERT(!constructing_);

    setCallee(callee);
    setThis(thisVal);

    MOZ_ASSERT(numActuals <= ArgumentsObject::MaxInlinedArgs);
    static_assert(
        ArgumentsObject::MaxInlinedArgs <= decltype(args_)::InlineLength,
        "Actual arguments can be infallibly stored inline");
    MOZ_ALWAYS_TRUE(args_.reserve(numActuals));
  }

  void popCallStack(MBasicBlock* current) { current->popn(numFormals()); }

  [[nodiscard]] bool pushCallStack(MBasicBlock* current) {
    current->push(callee());
    current->push(thisArg());

    for (uint32_t i = 0; i < argc(); i++) {
      current->push(getArg(i));
    }

    if (constructing()) {
      current->push(getNewTarget());
    }

    return true;
  }

  uint32_t argc() const { return args_.length(); }
  uint32_t numFormals() const { return argc() + 2 + constructing(); }

  [[nodiscard]] bool setArgs(const MDefinitionVector& args) {
    MOZ_ASSERT(args_.empty());
    return args_.appendAll(args);
  }

  MDefinitionVector& argv() { return args_; }

  const MDefinitionVector& argv() const { return args_; }

  MDefinition* getArg(uint32_t i) const {
    MOZ_ASSERT(i < argc());
    return args_[i];
  }

  void initArg(uint32_t i, MDefinition* def) {
    MOZ_ASSERT(i == argc());
    args_.infallibleAppend(def);
  }

  void setArg(uint32_t i, MDefinition* def) {
    MOZ_ASSERT(i < argc());
    args_[i] = def;
  }

  void removeArg(uint32_t i) { args_.erase(&args_[i]); }

  MDefinition* thisArg() const {
    MOZ_ASSERT(thisArg_);
    return thisArg_;
  }

  void setThis(MDefinition* thisArg) { thisArg_ = thisArg; }

  bool constructing() const { return constructing_; }

  bool ignoresReturnValue() const { return ignoresReturnValue_; }

  void setNewTarget(MDefinition* newTarget) {
    MOZ_ASSERT(constructing());
    newTargetArg_ = newTarget;
  }
  MDefinition* getNewTarget() const {
    MOZ_ASSERT(newTargetArg_);
    return newTargetArg_;
  }

  bool isSetter() const { return setter_; }
  void markAsSetter() { setter_ = true; }

  bool isInlined() const { return inlined_; }
  void markAsInlined() { inlined_ = true; }

  MDefinition* callee() const {
    MOZ_ASSERT(callee_);
    return callee_;
  }

  void setCallee(MDefinition* callee) { callee_ = callee; }

  template <typename Fun>
  void forEachCallOperand(Fun& f) {
    f(callee_);
    f(thisArg_);
    if (newTargetArg_) {
      f(newTargetArg_);
    }
    for (uint32_t i = 0; i < argc(); i++) {
      f(getArg(i));
    }
  }

  void setImplicitlyUsedUnchecked() {
    auto setFlag = [](MDefinition* def) { def->setImplicitlyUsedUnchecked(); };
    forEachCallOperand(setFlag);
  }

  ArgFormat argFormat() const { return argFormat_; }
  void setArgFormat(ArgFormat argFormat) { argFormat_ = argFormat; }

  MDefinition* arrayArg() const {
    MOZ_ASSERT(argFormat_ == ArgFormat::Array);
    // The array argument for a spread call or FunApply is always the last
    // argument, unless the spread call is constructing, in which case the
    // last argument is NewTarget, and the array argument is second-last.
    return getArg(argc() - 1 - constructing_);
  }
};

template <typename Undef>
MCall* MakeCall(TempAllocator& alloc, Undef addUndefined, CallInfo& callInfo,
                bool needsThisCheck, WrappedFunction* target, bool isDOMCall) {
  MOZ_ASSERT(callInfo.argFormat() == CallInfo::ArgFormat::Standard);
  MOZ_ASSERT_IF(needsThisCheck, !target);
  MOZ_ASSERT_IF(isDOMCall, target->jitInfo()->type() == JSJitInfo::Method);

  DOMObjectKind objKind = DOMObjectKind::Unknown;
  if (isDOMCall) {
    const JSClass* clasp = callInfo.thisArg()->toGuardToClass()->getClass();
    MOZ_ASSERT(clasp->isDOMClass());
    if (clasp->isNativeObject()) {
      objKind = DOMObjectKind::Native;
    } else {
      MOZ_ASSERT(clasp->isProxyObject());
      objKind = DOMObjectKind::Proxy;
    }
  }

  uint32_t targetArgs = callInfo.argc();

  // Collect number of missing arguments provided that the target is
  // scripted. Native functions are passed an explicit 'argc' parameter.
  if (target && target->hasJitEntry()) {
    targetArgs = std::max<uint32_t>(target->nargs(), callInfo.argc());
  }

  MCall* call =
      MCall::New(alloc, target, targetArgs + 1 + callInfo.constructing(),
                 callInfo.argc(), callInfo.constructing(),
                 callInfo.ignoresReturnValue(), isDOMCall, objKind);
  if (!call) {
    return nullptr;
  }

  if (callInfo.constructing()) {
    // Note: setThis should have been done by the caller of makeCall.
    if (needsThisCheck) {
      call->setNeedsThisCheck();
    }

    // Pass |new.target|
    call->addArg(targetArgs + 1, callInfo.getNewTarget());
  }

  // Explicitly pad any missing arguments with |undefined|.
  // This permits skipping the argumentsRectifier.
  MOZ_ASSERT_IF(target && targetArgs > callInfo.argc(), target->hasJitEntry());

  MConstant* undef = nullptr;
  for (uint32_t i = targetArgs; i > callInfo.argc(); i--) {
    if (!undef) {
      undef = addUndefined();
    }
    if (!alloc.ensureBallast()) {
      return nullptr;
    }
    call->addArg(i, undef);
  }

  // Add explicit arguments.
  // Skip addArg(0) because it is reserved for |this|.
  for (int32_t i = callInfo.argc() - 1; i >= 0; i--) {
    call->addArg(i + 1, callInfo.getArg(i));
  }

  if (isDOMCall) {
    // Now that we've told it about all the args, compute whether it's movable
    call->computeMovable();
  }

  // Pass |this| and callee.
  call->addArg(0, callInfo.thisArg());
  call->initCallee(callInfo.callee());

  if (target) {
    // The callee must be a JSFunction so we don't need a Class check.
    call->disableClassCheck();
  }

  return call;
}

// Base class for code sharing between WarpBuilder and WarpCacheIRTranspiler.
// Because this code is used by WarpCacheIRTranspiler we should
// generally assume that we only have access to the current basic block.
class WarpBuilderShared {
  WarpSnapshot& snapshot_;
  MIRGenerator& mirGen_;
  TempAllocator& alloc_;

 protected:
  MBasicBlock* current;

  WarpBuilderShared(WarpSnapshot& snapshot, MIRGenerator& mirGen,
                    MBasicBlock* current_);

  [[nodiscard]] bool resumeAfter(MInstruction* ins, BytecodeLocation loc);

  MConstant* constant(const JS::Value& v);
  void pushConstant(const JS::Value& v);

  MCall* makeCall(CallInfo& callInfo, bool needsThisCheck,
                  WrappedFunction* target = nullptr, bool isDOMCall = false);
  MInstruction* makeSpreadCall(CallInfo& callInfo, bool isSameRealm = false,
                               WrappedFunction* target = nullptr);

 public:
  MBasicBlock* currentBlock() const { return current; }
  WarpSnapshot& snapshot() const { return snapshot_; }
  MIRGenerator& mirGen() { return mirGen_; }
  TempAllocator& alloc() { return alloc_; }
};

}  // namespace jit
}  // namespace js

#endif

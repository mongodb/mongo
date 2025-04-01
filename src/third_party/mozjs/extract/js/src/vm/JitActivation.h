/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JitActivation_h
#define vm_JitActivation_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Atomics.h"     // mozilla::Atomic, mozilla::Relaxed
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t, uintptr_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "jit/IonTypes.h"        // CHECK_OSIPOINT_REGISTERS
#include "jit/JSJitFrameIter.h"  // js::jit::{JSJitFrameIter,RInstructionResults}
#ifdef CHECK_OSIPOINT_REGISTERS
#  include "jit/Registers.h"  // js::jit::RegisterDump
#endif
#include "jit/RematerializedFrame.h"  // js::jit::RematerializedFrame
#include "js/GCVector.h"              // JS::GCVector
#include "js/HashTable.h"             // js::HashMap
#include "js/UniquePtr.h"             // js::UniquePtr
#include "vm/Activation.h"            // js::Activation
#include "wasm/WasmCodegenTypes.h"    // js::wasm::TrapData
#include "wasm/WasmConstants.h"       // js::wasm::Trap
#include "wasm/WasmFrame.h"           // js::wasm::Frame
#include "wasm/WasmFrameIter.h"  // js::wasm::{ExitReason,RegisterState,WasmFrameIter}

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSTracer;

namespace js {

namespace jit {

class BailoutFrameInfo;

enum class IsLeavingFrame { No, Yes };

// A JitActivation is used for frames running in Baseline or Ion.
class JitActivation : public Activation {
  // If Baseline, Ion or Wasm code is on the stack, and has called into C++,
  // this will be aligned to an ExitFrame. The last bit indicates if it's a
  // wasm frame (bit set to wasm::ExitOrJitEntryFPTag) or not
  // (bit set to ~wasm::ExitOrJitEntryFPTag).
  uint8_t* packedExitFP_;

  // When hasWasmExitFP(), encodedWasmExitReason_ holds ExitReason.
  uint32_t encodedWasmExitReason_;

  JitActivation* prevJitActivation_;

  // Rematerialized Ion frames which has info copied out of snapshots. Maps
  // frame pointers (i.e. packedExitFP_) to a vector of rematerializations of
  // all inline frames associated with that frame.
  //
  // This table is lazily initialized by calling getRematerializedFrame.
  using RematerializedFrameVector =
      JS::GCVector<js::UniquePtr<RematerializedFrame>>;
  using RematerializedFrameTable =
      js::HashMap<uint8_t*, RematerializedFrameVector>;
  js::UniquePtr<RematerializedFrameTable> rematerializedFrames_;

  // This vector is used to remember the outcome of the evaluation of recover
  // instructions.
  //
  // RInstructionResults are appended into this vector when Snapshot values
  // have to be read, or when the evaluation has to run before some mutating
  // code.  Each RInstructionResults belongs to one frame which has to bailout
  // as soon as we get back to it.
  using IonRecoveryMap = Vector<RInstructionResults, 1>;
  IonRecoveryMap ionRecovery_;

  // If we are bailing out from Ion, then this field should be a non-null
  // pointer which references the BailoutFrameInfo used to walk the inner
  // frames. This field is used for all newly constructed JSJitFrameIters to
  // read the innermost frame information from this bailout data instead of
  // reading it from the stack.
  BailoutFrameInfo* bailoutData_;

  // When profiling is enabled, these fields will be updated to reflect the
  // last pushed frame for this activation, and if that frame has been
  // left for a call, the native code site of the call.
  mozilla::Atomic<JitFrameLayout*, mozilla::Relaxed> lastProfilingFrame_;
  mozilla::Atomic<void*, mozilla::Relaxed> lastProfilingCallSite_;
  static_assert(sizeof(mozilla::Atomic<void*, mozilla::Relaxed>) ==
                    sizeof(void*),
                "Atomic should have same memory format as underlying type.");

  // When wasm traps, the signal handler records some data for unwinding
  // purposes. Wasm code can't trap reentrantly.
  mozilla::Maybe<wasm::TrapData> wasmTrapData_;

#ifdef CHECK_OSIPOINT_REGISTERS
 protected:
  // Used to verify that live registers don't change between a VM call and
  // the OsiPoint that follows it. Protected to silence Clang warning.
  uint32_t checkRegs_ = 0;
  RegisterDump regs_;
#endif

 public:
  explicit JitActivation(JSContext* cx);
  ~JitActivation();

  bool isProfiling() const {
    // All JitActivations can be profiled.
    return true;
  }

  JitActivation* prevJitActivation() const { return prevJitActivation_; }
  static size_t offsetOfPrevJitActivation() {
    return offsetof(JitActivation, prevJitActivation_);
  }

  bool hasExitFP() const { return !!packedExitFP_; }
  uint8_t* jsOrWasmExitFP() const {
    if (hasWasmExitFP()) {
      return wasm::Frame::untagExitFP(packedExitFP_);
    }
    return packedExitFP_;
  }
  static size_t offsetOfPackedExitFP() {
    return offsetof(JitActivation, packedExitFP_);
  }

  bool hasJSExitFP() const { return !hasWasmExitFP(); }

  uint8_t* jsExitFP() const {
    MOZ_ASSERT(hasJSExitFP());
    return packedExitFP_;
  }
  void setJSExitFP(uint8_t* fp) { packedExitFP_ = fp; }

  uint8_t* packedExitFP() const { return packedExitFP_; }
  void setPackedExitFP(uint8_t* fp) { packedExitFP_ = fp; }

#ifdef CHECK_OSIPOINT_REGISTERS
  void setCheckRegs(bool check) { checkRegs_ = check; }
  static size_t offsetOfCheckRegs() {
    return offsetof(JitActivation, checkRegs_);
  }
  static size_t offsetOfRegs() { return offsetof(JitActivation, regs_); }
#endif

  // Look up a rematerialized frame keyed by the fp, rematerializing the
  // frame if one doesn't already exist. A frame can only be rematerialized
  // if an IonFrameIterator pointing to the nearest uninlined frame can be
  // provided, as values need to be read out of snapshots.
  //
  // The inlineDepth must be within bounds of the frame pointed to by iter.
  RematerializedFrame* getRematerializedFrame(
      JSContext* cx, const JSJitFrameIter& iter, size_t inlineDepth = 0,
      IsLeavingFrame leaving = IsLeavingFrame::No);

  // Look up a rematerialized frame by the fp. If inlineDepth is out of
  // bounds of what has been rematerialized, nullptr is returned.
  RematerializedFrame* lookupRematerializedFrame(uint8_t* top,
                                                 size_t inlineDepth = 0);

  // Remove all rematerialized frames associated with the fp top from the
  // Debugger.
  void removeRematerializedFramesFromDebugger(JSContext* cx, uint8_t* top);

  bool hasRematerializedFrame(uint8_t* top, size_t inlineDepth = 0) {
    return !!lookupRematerializedFrame(top, inlineDepth);
  }

  // Remove a previous rematerialization by fp.
  void removeRematerializedFrame(uint8_t* top);

  void traceRematerializedFrames(JSTracer* trc);

  // Register the results of on Ion frame recovery.
  bool registerIonFrameRecovery(RInstructionResults&& results);

  // Return the pointer to the Ion frame recovery, if it is already registered.
  RInstructionResults* maybeIonFrameRecovery(JitFrameLayout* fp);

  // If an Ion frame recovery exists for the |fp| frame exists, then remove it
  // from the activation.
  void removeIonFrameRecovery(JitFrameLayout* fp);

  void traceIonRecovery(JSTracer* trc);

  // Return the bailout information if it is registered.
  const BailoutFrameInfo* bailoutData() const { return bailoutData_; }

  // Register the bailout data when it is constructed.
  void setBailoutData(BailoutFrameInfo* bailoutData);

  // Unregister the bailout data when the frame is reconstructed.
  void cleanBailoutData();

  static size_t offsetOfLastProfilingFrame() {
    return offsetof(JitActivation, lastProfilingFrame_);
  }
  JitFrameLayout* lastProfilingFrame() { return lastProfilingFrame_; }
  void setLastProfilingFrame(JitFrameLayout* ptr) { lastProfilingFrame_ = ptr; }

  static size_t offsetOfLastProfilingCallSite() {
    return offsetof(JitActivation, lastProfilingCallSite_);
  }
  void* lastProfilingCallSite() { return lastProfilingCallSite_; }
  void setLastProfilingCallSite(void* ptr) { lastProfilingCallSite_ = ptr; }

  // WebAssembly specific attributes.
  bool hasWasmExitFP() const { return wasm::Frame::isExitFP(packedExitFP_); }
  wasm::Frame* wasmExitFP() const {
    MOZ_ASSERT(hasWasmExitFP());
    return reinterpret_cast<wasm::Frame*>(
        wasm::Frame::untagExitFP(packedExitFP_));
  }
  wasm::Instance* wasmExitInstance() const {
    return wasm::GetNearestEffectiveInstance(wasmExitFP());
  }
  void setWasmExitFP(const wasm::Frame* fp) {
    if (fp) {
      MOZ_ASSERT(!wasm::Frame::isExitFP(fp));
      packedExitFP_ = wasm::Frame::addExitFPTag(fp);
      MOZ_ASSERT(hasWasmExitFP());
    } else {
      packedExitFP_ = nullptr;
    }
  }
  wasm::ExitReason wasmExitReason() const {
    MOZ_ASSERT(hasWasmExitFP());
    return wasm::ExitReason::Decode(encodedWasmExitReason_);
  }
  static size_t offsetOfEncodedWasmExitReason() {
    return offsetof(JitActivation, encodedWasmExitReason_);
  }

  void startWasmTrap(wasm::Trap trap, uint32_t bytecodeOffset,
                     const wasm::RegisterState& state);
  void finishWasmTrap();
  bool isWasmTrapping() const { return !!wasmTrapData_; }
  const wasm::TrapData& wasmTrapData() { return *wasmTrapData_; }
};

// A filtering of the ActivationIterator to only stop at JitActivations.
class JitActivationIterator : public ActivationIterator {
  void settle() {
    while (!done() && !activation_->isJit()) {
      ActivationIterator::operator++();
    }
  }

 public:
  explicit JitActivationIterator(JSContext* cx) : ActivationIterator(cx) {
    settle();
  }

  JitActivationIterator& operator++() {
    ActivationIterator::operator++();
    settle();
    return *this;
  }
};

}  // namespace jit

}  // namespace js

#endif  // vm_JitActivation_h

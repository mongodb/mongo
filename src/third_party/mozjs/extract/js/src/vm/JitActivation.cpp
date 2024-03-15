/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/JitActivation.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_RELEASE_ASSERT

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t
#include <utility>   // std::move

#include "debugger/DebugAPI.h"        // js::DebugAPI
#include "jit/JSJitFrameIter.h"       // js::jit::InlineFrameIterator
#include "jit/RematerializedFrame.h"  // js::jit::RematerializedFrame
#include "js/AllocPolicy.h"           // js::ReportOutOfMemory
#include "vm/EnvironmentObject.h"     // js::DebugEnvironments
#include "vm/JSContext.h"             // JSContext
#include "vm/Realm.h"                 // js::AutoRealmUnchecked
#include "wasm/WasmCode.h"            // js::wasm::Code
#include "wasm/WasmConstants.h"       // js::wasm::Trap
#include "wasm/WasmFrameIter.h"  // js::wasm::{RegisterState,StartUnwinding,UnwindState}
#include "wasm/WasmInstance.h"  // js::wasm::Instance
#include "wasm/WasmProcess.h"   // js::wasm::LookupCode

#include "vm/Realm-inl.h"  // js::~AutoRealm

class JS_PUBLIC_API JSTracer;

js::jit::JitActivation::JitActivation(JSContext* cx)
    : Activation(cx, Jit),
      packedExitFP_(nullptr),
      encodedWasmExitReason_(0),
      prevJitActivation_(cx->jitActivation),
      rematerializedFrames_(),
      ionRecovery_(cx),
      bailoutData_(nullptr),
      lastProfilingFrame_(nullptr),
      lastProfilingCallSite_(nullptr) {
  cx->jitActivation = this;
  registerProfiling();
}

js::jit::JitActivation::~JitActivation() {
  if (isProfiling()) {
    unregisterProfiling();
  }
  cx_->jitActivation = prevJitActivation_;

  // All reocvered value are taken from activation during the bailout.
  MOZ_ASSERT(ionRecovery_.empty());

  // The BailoutFrameInfo should have unregistered itself from the
  // JitActivations.
  MOZ_ASSERT(!bailoutData_);

  // Traps get handled immediately.
  MOZ_ASSERT(!isWasmTrapping());

  clearRematerializedFrames();
}

void js::jit::JitActivation::setBailoutData(
    jit::BailoutFrameInfo* bailoutData) {
  MOZ_ASSERT(!bailoutData_);
  bailoutData_ = bailoutData;
}

void js::jit::JitActivation::cleanBailoutData() {
  MOZ_ASSERT(bailoutData_);
  bailoutData_ = nullptr;
}

void js::jit::JitActivation::removeRematerializedFrame(uint8_t* top) {
  if (!rematerializedFrames_) {
    return;
  }

  if (RematerializedFrameTable::Ptr p = rematerializedFrames_->lookup(top)) {
    rematerializedFrames_->remove(p);
  }
}

void js::jit::JitActivation::clearRematerializedFrames() {
  if (!rematerializedFrames_) {
    return;
  }

  for (RematerializedFrameTable::Enum e(*rematerializedFrames_); !e.empty();
       e.popFront()) {
    e.removeFront();
  }
}

js::jit::RematerializedFrame* js::jit::JitActivation::getRematerializedFrame(
    JSContext* cx, const JSJitFrameIter& iter, size_t inlineDepth,
    MaybeReadFallback::FallbackConsequence consequence) {
  MOZ_ASSERT(iter.activation() == this);
  MOZ_ASSERT(iter.isIonScripted());

  if (!rematerializedFrames_) {
    rematerializedFrames_ = cx->make_unique<RematerializedFrameTable>(cx);
    if (!rematerializedFrames_) {
      return nullptr;
    }
  }

  uint8_t* top = iter.fp();
  RematerializedFrameTable::AddPtr p = rematerializedFrames_->lookupForAdd(top);
  if (!p) {
    RematerializedFrameVector frames(cx);

    // The unit of rematerialization is an uninlined frame and its inlined
    // frames. Since inlined frames do not exist outside of snapshots, it
    // is impossible to synchronize their rematerialized copies to
    // preserve identity. Therefore, we always rematerialize an uninlined
    // frame and all its inlined frames at once.
    InlineFrameIterator inlineIter(cx, &iter);
    MaybeReadFallback recover(cx, this, &iter, consequence);

    // Frames are often rematerialized with the cx inside a Debugger's
    // realm. To recover slots and to create CallObjects, we need to
    // be in the script's realm.
    AutoRealmUnchecked ar(cx, iter.script()->realm());

    if (!RematerializedFrame::RematerializeInlineFrames(cx, top, inlineIter,
                                                        recover, frames)) {
      return nullptr;
    }

    if (!rematerializedFrames_->add(p, top, std::move(frames))) {
      ReportOutOfMemory(cx);
      return nullptr;
    }

    // See comment in unsetPrevUpToDateUntil.
    DebugEnvironments::unsetPrevUpToDateUntil(cx,
                                              p->value()[inlineDepth].get());
  }

  return p->value()[inlineDepth].get();
}

js::jit::RematerializedFrame* js::jit::JitActivation::lookupRematerializedFrame(
    uint8_t* top, size_t inlineDepth) {
  if (!rematerializedFrames_) {
    return nullptr;
  }
  if (RematerializedFrameTable::Ptr p = rematerializedFrames_->lookup(top)) {
    return inlineDepth < p->value().length() ? p->value()[inlineDepth].get()
                                             : nullptr;
  }
  return nullptr;
}

void js::jit::JitActivation::removeRematerializedFramesFromDebugger(
    JSContext* cx, uint8_t* top) {
  // Ion bailout can fail due to overrecursion and OOM. In such cases we
  // cannot honor any further Debugger hooks on the frame, and need to
  // ensure that its Debugger.Frame entry is cleaned up.
  if (!cx->realm()->isDebuggee() || !rematerializedFrames_) {
    return;
  }
  if (RematerializedFrameTable::Ptr p = rematerializedFrames_->lookup(top)) {
    for (uint32_t i = 0; i < p->value().length(); i++) {
      DebugAPI::handleUnrecoverableIonBailoutError(cx, p->value()[i].get());
    }
    rematerializedFrames_->remove(p);
  }
}

void js::jit::JitActivation::traceRematerializedFrames(JSTracer* trc) {
  if (!rematerializedFrames_) {
    return;
  }
  for (RematerializedFrameTable::Enum e(*rematerializedFrames_); !e.empty();
       e.popFront()) {
    e.front().value().trace(trc);
  }
}

bool js::jit::JitActivation::registerIonFrameRecovery(
    RInstructionResults&& results) {
  // Check that there is no entry in the vector yet.
  MOZ_ASSERT(!maybeIonFrameRecovery(results.frame()));
  if (!ionRecovery_.append(std::move(results))) {
    return false;
  }

  return true;
}

js::jit::RInstructionResults* js::jit::JitActivation::maybeIonFrameRecovery(
    JitFrameLayout* fp) {
  for (RInstructionResults* it = ionRecovery_.begin(); it != ionRecovery_.end();
       it++) {
    if (it->frame() == fp) {
      return it;
    }
  }

  return nullptr;
}

void js::jit::JitActivation::removeIonFrameRecovery(JitFrameLayout* fp) {
  RInstructionResults* elem = maybeIonFrameRecovery(fp);
  if (!elem) {
    return;
  }

  ionRecovery_.erase(elem);
}

void js::jit::JitActivation::traceIonRecovery(JSTracer* trc) {
  for (RInstructionResults* it = ionRecovery_.begin(); it != ionRecovery_.end();
       it++) {
    it->trace(trc);
  }
}

void js::jit::JitActivation::startWasmTrap(wasm::Trap trap,
                                           uint32_t bytecodeOffset,
                                           const wasm::RegisterState& state) {
  MOZ_ASSERT(!isWasmTrapping());

  bool unwound;
  wasm::UnwindState unwindState;
  MOZ_RELEASE_ASSERT(wasm::StartUnwinding(state, &unwindState, &unwound));
  MOZ_ASSERT(unwound == (trap == wasm::Trap::IndirectCallBadSig));

  void* pc = unwindState.pc;
  const wasm::Frame* fp = wasm::Frame::fromUntaggedWasmExitFP(unwindState.fp);

  const wasm::Code& code = wasm::GetNearestEffectiveInstance(fp)->code();
  MOZ_RELEASE_ASSERT(&code == wasm::LookupCode(pc));

  // If the frame was unwound, the bytecodeOffset must be recovered from the
  // callsite so that it is accurate.
  if (unwound) {
    bytecodeOffset = code.lookupCallSite(pc)->lineOrBytecode();
  }

  setWasmExitFP(fp);
  wasmTrapData_.emplace();
  wasmTrapData_->resumePC =
      ((uint8_t*)state.pc) + jit::WasmTrapInstructionLength;
  wasmTrapData_->unwoundPC = pc;
  wasmTrapData_->trap = trap;
  wasmTrapData_->bytecodeOffset = bytecodeOffset;

  MOZ_ASSERT(isWasmTrapping());
}

void js::jit::JitActivation::finishWasmTrap() {
  MOZ_ASSERT(isWasmTrapping());
  packedExitFP_ = nullptr;
  wasmTrapData_.reset();
  MOZ_ASSERT(!isWasmTrapping());
}

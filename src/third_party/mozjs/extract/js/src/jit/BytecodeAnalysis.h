/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BytecodeAnalysis_h
#define jit_BytecodeAnalysis_h

#include "jit/JitAllocPolicy.h"
#include "js/Vector.h"
#include "vm/JSScript.h"

namespace js {
namespace jit {

// Basic information about bytecodes in the script.  Used to help baseline
// compilation.
struct BytecodeInfo {
  static const uint16_t MAX_STACK_DEPTH = 0xffffU;
  uint16_t stackDepth;
  bool initialized : 1;
  bool jumpTarget : 1;

  // If true, this is a JSOp::LoopHead where we can OSR into Ion/Warp code.
  bool loopHeadCanOsr : 1;

  // See the comment above normallyReachable in BytecodeAnalysis.cpp for how
  // this works.
  bool jumpTargetNormallyReachable : 1;

  // True if the script has a resume offset for this bytecode op.
  bool hasResumeOffset : 1;

  void init(unsigned depth) {
    MOZ_ASSERT(depth <= MAX_STACK_DEPTH);
    MOZ_ASSERT_IF(initialized, stackDepth == depth);
    initialized = true;
    stackDepth = depth;
  }

  void setJumpTarget(bool normallyReachable) {
    jumpTarget = true;
    if (normallyReachable) {
      jumpTargetNormallyReachable = true;
    }
  }
};

class BytecodeAnalysis {
  JSScript* script_;
  Vector<BytecodeInfo, 0, JitAllocPolicy> infos_;

 public:
  explicit BytecodeAnalysis(TempAllocator& alloc, JSScript* script);

  [[nodiscard]] bool init(TempAllocator& alloc);

  BytecodeInfo& info(jsbytecode* pc) {
    uint32_t pcOffset = script_->pcToOffset(pc);
    MOZ_ASSERT(infos_[pcOffset].initialized);
    return infos_[pcOffset];
  }

  BytecodeInfo* maybeInfo(jsbytecode* pc) {
    uint32_t pcOffset = script_->pcToOffset(pc);
    if (infos_[pcOffset].initialized) {
      return &infos_[pcOffset];
    }
    return nullptr;
  }

  void checkWarpSupport(JSOp op);
};

// Bytecode analysis pass necessary for WarpBuilder. The result is cached in
// JitScript.
struct IonBytecodeInfo;
IonBytecodeInfo AnalyzeBytecodeForIon(JSContext* cx, JSScript* script);

}  // namespace jit
}  // namespace js

#endif /* jit_BytecodeAnalysis_h */

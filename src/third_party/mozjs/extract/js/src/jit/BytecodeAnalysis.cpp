/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BytecodeAnalysis.h"

#include "jit/JitSpewer.h"
#include "jit/WarpBuilder.h"
#include "vm/BytecodeIterator.h"
#include "vm/BytecodeLocation.h"
#include "vm/BytecodeUtil.h"

#include "vm/BytecodeIterator-inl.h"
#include "vm/BytecodeLocation-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

BytecodeAnalysis::BytecodeAnalysis(TempAllocator& alloc, JSScript* script)
    : script_(script), infos_(alloc) {}

bool BytecodeAnalysis::init(TempAllocator& alloc) {
  if (!infos_.growByUninitialized(script_->length())) {
    return false;
  }

  // Clear all BytecodeInfo.
  mozilla::PodZero(infos_.begin(), infos_.length());
  infos_[0].init(/*stackDepth=*/0);

  // Because WarpBuilder can compile try-blocks but doesn't compile the
  // catch-body, we need some special machinery to prevent OSR into Warp code in
  // the following cases:
  //
  // (1) Loops in catch/finally blocks:
  //
  //       try {
  //         ..
  //       } catch (e) {
  //         while (..) {} // Can't OSR here.
  //       }
  //
  // (2) Loops only reachable via a catch/finally block:
  //
  //       for (;;) {
  //         try {
  //           throw 3;
  //         } catch (e) {
  //           break;
  //         }
  //       }
  //       while (..) {} // Loop is only reachable via the catch-block.
  //
  // To deal with both of these cases, we track whether the current op is
  // 'normally reachable' (reachable without going through a catch/finally
  // block). Forward jumps propagate this flag to their jump targets (see
  // BytecodeInfo::jumpTargetNormallyReachable) and when the analysis reaches a
  // jump target it updates its normallyReachable flag based on the target's
  // flag.
  //
  // Inlining a function without a normally reachable return can cause similar
  // problems. To avoid this, we mark such functions as uninlineable.
  bool normallyReachable = true;
  bool normallyReachableReturn = false;

  for (const BytecodeLocation& it : AllBytecodesIterable(script_)) {
    JSOp op = it.getOp();
    uint32_t offset = it.bytecodeToOffset(script_);

    JitSpew(JitSpew_BaselineOp, "Analyzing op @ %u (end=%u): %s",
            unsigned(offset), unsigned(script_->length()), CodeName(op));

    checkWarpSupport(op);

    // If this bytecode info has not yet been initialized, it's not reachable.
    if (!infos_[offset].initialized) {
      continue;
    }

    uint32_t stackDepth = infos_[offset].stackDepth;

    if (infos_[offset].jumpTarget) {
      normallyReachable = infos_[offset].jumpTargetNormallyReachable;
    }

#ifdef DEBUG
    size_t endOffset = offset + it.length();
    for (size_t checkOffset = offset + 1; checkOffset < endOffset;
         checkOffset++) {
      MOZ_ASSERT(!infos_[checkOffset].initialized);
    }
#endif
    uint32_t nuses = it.useCount();
    uint32_t ndefs = it.defCount();

    MOZ_ASSERT(stackDepth >= nuses);
    stackDepth -= nuses;
    stackDepth += ndefs;

    // If stack depth exceeds max allowed by analysis, fail fast.
    MOZ_ASSERT(stackDepth <= BytecodeInfo::MAX_STACK_DEPTH);

    switch (op) {
      case JSOp::TableSwitch: {
        uint32_t defaultOffset = it.getTableSwitchDefaultOffset(script_);
        int32_t low = it.getTableSwitchLow();
        int32_t high = it.getTableSwitchHigh();

        infos_[defaultOffset].init(stackDepth);
        infos_[defaultOffset].setJumpTarget(normallyReachable);

        uint32_t ncases = high - low + 1;

        for (uint32_t i = 0; i < ncases; i++) {
          uint32_t targetOffset = it.tableSwitchCaseOffset(script_, i);
          if (targetOffset != defaultOffset) {
            infos_[targetOffset].init(stackDepth);
            infos_[targetOffset].setJumpTarget(normallyReachable);
          }
        }
        break;
      }

      case JSOp::Try: {
        for (const TryNote& tn : script_->trynotes()) {
          if (tn.start == offset + JSOpLength_Try &&
              (tn.kind() == TryNoteKind::Catch ||
               tn.kind() == TryNoteKind::Finally)) {
            uint32_t catchOrFinallyOffset = tn.start + tn.length;
            BytecodeInfo& targetInfo = infos_[catchOrFinallyOffset];
            targetInfo.init(stackDepth);
            targetInfo.setJumpTarget(/* normallyReachable = */ false);
          }
        }
        break;
      }

      case JSOp::LoopHead:
        infos_[offset].loopHeadCanOsr = normallyReachable;
        break;

#ifdef DEBUG
      case JSOp::Exception:
      case JSOp::Finally:
        // Sanity check: ops only emitted in catch/finally blocks are never
        // normally reachable.
        MOZ_ASSERT(!normallyReachable);
        break;
#endif

      case JSOp::Return:
      case JSOp::RetRval:
        if (normallyReachable) {
          normallyReachableReturn = true;
        }
        break;

      default:
        break;
    }

    bool jump = it.isJump();
    if (jump) {
      // Case instructions do not push the lvalue back when branching.
      uint32_t newStackDepth = stackDepth;
      if (it.is(JSOp::Case)) {
        newStackDepth--;
      }

      uint32_t targetOffset = it.getJumpTargetOffset(script_);

#ifdef DEBUG
      // If this is a backedge, the target JSOp::LoopHead must have been
      // analyzed already. Furthermore, if the backedge is normally reachable,
      // the loop head must be normally reachable too (loopHeadCanOsr can be
      // used to check this since it's equivalent).
      if (targetOffset < offset) {
        MOZ_ASSERT(infos_[targetOffset].initialized);
        MOZ_ASSERT_IF(normallyReachable, infos_[targetOffset].loopHeadCanOsr);
      }
#endif

      infos_[targetOffset].init(newStackDepth);

      // Gosub's target is a finally-block => not normally reachable.
      bool targetNormallyReachable = (op != JSOp::Gosub) && normallyReachable;
      infos_[targetOffset].setJumpTarget(targetNormallyReachable);
    }

    // Handle any fallthrough from this opcode.
    if (it.fallsThrough()) {
      BytecodeLocation fallthroughLoc = it.next();
      MOZ_ASSERT(fallthroughLoc.isInBounds(script_));
      uint32_t fallthroughOffset = fallthroughLoc.bytecodeToOffset(script_);

      infos_[fallthroughOffset].init(stackDepth);

      // Treat the fallthrough of a branch instruction as a jump target.
      if (jump) {
        // Gosub falls through after executing a finally-block => not normally
        // reachable.
        bool nextNormallyReachable = (op != JSOp::Gosub) && normallyReachable;
        infos_[fallthroughOffset].setJumpTarget(nextNormallyReachable);
      }
    }
  }

  // Flag (reachable) resume offset instructions.
  for (uint32_t offset : script_->resumeOffsets()) {
    BytecodeInfo& info = infos_[offset];
    if (info.initialized) {
      info.hasResumeOffset = true;
    }
  }

  if (!normallyReachableReturn) {
    script_->setUninlineable();
  }

  return true;
}

void BytecodeAnalysis::checkWarpSupport(JSOp op) {
  switch (op) {
#define DEF_CASE(OP) case JSOp::OP:
    WARP_UNSUPPORTED_OPCODE_LIST(DEF_CASE)
#undef DEF_CASE
    if (script_->canIonCompile()) {
      JitSpew(JitSpew_IonAbort, "Disabling Warp support for %s:%d:%d due to %s",
              script_->filename(), script_->lineno(), script_->column(),
              CodeName(op));
      script_->disableIon();
    }
    break;
    default:
      break;
  }
}

IonBytecodeInfo js::jit::AnalyzeBytecodeForIon(JSContext* cx,
                                               JSScript* script) {
  IonBytecodeInfo result;

  if (script->isModule() || script->initialEnvironmentShape() ||
      (script->function() &&
       script->function()->needsSomeEnvironmentObject())) {
    result.usesEnvironmentChain = true;
  }

  AllBytecodesIterable iterator(script);

  for (const BytecodeLocation& location : iterator) {
    switch (location.getOp()) {
      case JSOp::SetArg:
        result.modifiesArguments = true;
        break;

      case JSOp::GetName:
      case JSOp::BindName:
      case JSOp::BindVar:
      case JSOp::SetName:
      case JSOp::StrictSetName:
      case JSOp::DelName:
      case JSOp::GetAliasedVar:
      case JSOp::SetAliasedVar:
      case JSOp::Lambda:
      case JSOp::LambdaArrow:
      case JSOp::PushLexicalEnv:
      case JSOp::PopLexicalEnv:
      case JSOp::ImplicitThis:
      case JSOp::FunWithProto:
      case JSOp::GlobalOrEvalDeclInstantiation:
        result.usesEnvironmentChain = true;
        break;

      case JSOp::GetGName:
      case JSOp::SetGName:
      case JSOp::StrictSetGName:
      case JSOp::GImplicitThis:
        if (script->hasNonSyntacticScope()) {
          result.usesEnvironmentChain = true;
        }
        break;

      case JSOp::Finally:
        result.hasTryFinally = true;
        break;

      default:
        break;
    }
  }

  return result;
}

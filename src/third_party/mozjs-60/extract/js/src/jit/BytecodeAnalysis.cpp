/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BytecodeAnalysis.h"

#include "jit/JitSpewer.h"
#include "vm/BytecodeUtil.h"

#include "vm/BytecodeUtil-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

BytecodeAnalysis::BytecodeAnalysis(TempAllocator& alloc, JSScript* script)
  : script_(script),
    infos_(alloc),
    usesEnvironmentChain_(false)
{
}

// Bytecode range containing only catch or finally code.
struct CatchFinallyRange
{
    uint32_t start; // Inclusive.
    uint32_t end;   // Exclusive.

    CatchFinallyRange(uint32_t start, uint32_t end)
      : start(start), end(end)
    {
        MOZ_ASSERT(end > start);
    }

    bool contains(uint32_t offset) const {
        return start <= offset && offset < end;
    }
};

bool
BytecodeAnalysis::init(TempAllocator& alloc, GSNCache& gsn)
{
    if (!infos_.growByUninitialized(script_->length()))
        return false;

    // Initialize the env chain slot if either the function needs some
    // EnvironmentObject (like a CallObject) or the script uses the env
    // chain. The latter case is handled below.
    usesEnvironmentChain_ = script_->module() || script_->initialEnvironmentShape() ||
                            (script_->functionDelazifying() &&
                             script_->functionDelazifying()->needsSomeEnvironmentObject());

    jsbytecode* end = script_->codeEnd();

    // Clear all BytecodeInfo.
    mozilla::PodZero(infos_.begin(), infos_.length());
    infos_[0].init(/*stackDepth=*/0);

    Vector<CatchFinallyRange, 0, JitAllocPolicy> catchFinallyRanges(alloc);

    jsbytecode* nextpc;
    for (jsbytecode* pc = script_->code(); pc < end; pc = nextpc) {
        JSOp op = JSOp(*pc);
        nextpc = pc + GetBytecodeLength(pc);
        unsigned offset = script_->pcToOffset(pc);

        JitSpew(JitSpew_BaselineOp, "Analyzing op @ %d (end=%d): %s",
                int(script_->pcToOffset(pc)), int(script_->length()), CodeName[op]);

        // If this bytecode info has not yet been initialized, it's not reachable.
        if (!infos_[offset].initialized)
            continue;

        unsigned stackDepth = infos_[offset].stackDepth;
#ifdef DEBUG
        for (jsbytecode* chkpc = pc + 1; chkpc < (pc + GetBytecodeLength(pc)); chkpc++)
            MOZ_ASSERT(!infos_[script_->pcToOffset(chkpc)].initialized);
#endif

        unsigned nuses = GetUseCount(pc);
        unsigned ndefs = GetDefCount(pc);

        MOZ_ASSERT(stackDepth >= nuses);
        stackDepth -= nuses;
        stackDepth += ndefs;

        // If stack depth exceeds max allowed by analysis, fail fast.
        MOZ_ASSERT(stackDepth <= BytecodeInfo::MAX_STACK_DEPTH);

        switch (op) {
          case JSOP_TABLESWITCH: {
            unsigned defaultOffset = offset + GET_JUMP_OFFSET(pc);
            jsbytecode* pc2 = pc + JUMP_OFFSET_LEN;
            int32_t low = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;
            int32_t high = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;

            infos_[defaultOffset].init(stackDepth);
            infos_[defaultOffset].jumpTarget = true;

            for (int32_t i = low; i <= high; i++) {
                unsigned targetOffset = offset + GET_JUMP_OFFSET(pc2);
                if (targetOffset != offset) {
                    infos_[targetOffset].init(stackDepth);
                    infos_[targetOffset].jumpTarget = true;
                }
                pc2 += JUMP_OFFSET_LEN;
            }
            break;
          }

          case JSOP_TRY: {
            JSTryNote* tn = script_->trynotes()->vector;
            JSTryNote* tnlimit = tn + script_->trynotes()->length;
            for (; tn < tnlimit; tn++) {
                unsigned startOffset = script_->mainOffset() + tn->start;
                if (startOffset == offset + 1) {
                    unsigned catchOffset = startOffset + tn->length;

                    if (tn->kind != JSTRY_FOR_IN) {
                        infos_[catchOffset].init(stackDepth);
                        infos_[catchOffset].jumpTarget = true;
                    }
                }
            }

            // Get the pc of the last instruction in the try block. It's a JSOP_GOTO to
            // jump over the catch/finally blocks.
            jssrcnote* sn = GetSrcNote(gsn, script_, pc);
            MOZ_ASSERT(SN_TYPE(sn) == SRC_TRY);

            jsbytecode* endOfTry = pc + GetSrcNoteOffset(sn, 0);
            MOZ_ASSERT(JSOp(*endOfTry) == JSOP_GOTO);

            jsbytecode* afterTry = endOfTry + GET_JUMP_OFFSET(endOfTry);
            MOZ_ASSERT(afterTry > endOfTry);

            // Ensure the code following the try-block is always marked as
            // reachable, to simplify Ion's ControlFlowGenerator.
            uint32_t afterTryOffset = script_->pcToOffset(afterTry);
            infos_[afterTryOffset].init(stackDepth);
            infos_[afterTryOffset].jumpTarget = true;

            // Pop CatchFinallyRanges that are no longer needed.
            while (!catchFinallyRanges.empty() && catchFinallyRanges.back().end <= offset)
                catchFinallyRanges.popBack();

            CatchFinallyRange range(script_->pcToOffset(endOfTry), script_->pcToOffset(afterTry));
            if (!catchFinallyRanges.append(range))
                return false;
            break;
          }

          case JSOP_LOOPENTRY:
            for (size_t i = 0; i < catchFinallyRanges.length(); i++) {
                if (catchFinallyRanges[i].contains(offset))
                    infos_[offset].loopEntryInCatchOrFinally = true;
            }
            break;

          case JSOP_GETNAME:
          case JSOP_BINDNAME:
          case JSOP_BINDVAR:
          case JSOP_SETNAME:
          case JSOP_STRICTSETNAME:
          case JSOP_DELNAME:
          case JSOP_GETALIASEDVAR:
          case JSOP_SETALIASEDVAR:
          case JSOP_LAMBDA:
          case JSOP_LAMBDA_ARROW:
          case JSOP_DEFFUN:
          case JSOP_DEFVAR:
          case JSOP_PUSHLEXICALENV:
          case JSOP_POPLEXICALENV:
          case JSOP_IMPLICITTHIS:
            usesEnvironmentChain_ = true;
            break;

          case JSOP_GETGNAME:
          case JSOP_SETGNAME:
          case JSOP_STRICTSETGNAME:
          case JSOP_GIMPLICITTHIS:
            if (script_->hasNonSyntacticScope())
                usesEnvironmentChain_ = true;
            break;

          default:
            break;
        }

        bool jump = IsJumpOpcode(op);
        if (jump) {
            // Case instructions do not push the lvalue back when branching.
            unsigned newStackDepth = stackDepth;
            if (op == JSOP_CASE)
                newStackDepth--;

            unsigned targetOffset = offset + GET_JUMP_OFFSET(pc);

            // If this is a a backedge to an un-analyzed segment, analyze from there.
            bool jumpBack = (targetOffset < offset) && !infos_[targetOffset].initialized;

            infos_[targetOffset].init(newStackDepth);
            infos_[targetOffset].jumpTarget = true;

            if (jumpBack)
                nextpc = script_->offsetToPC(targetOffset);
        }

        // Handle any fallthrough from this opcode.
        if (BytecodeFallsThrough(op)) {
            jsbytecode* fallthrough = pc + GetBytecodeLength(pc);
            MOZ_ASSERT(fallthrough < end);
            unsigned fallthroughOffset = script_->pcToOffset(fallthrough);

            infos_[fallthroughOffset].init(stackDepth);

            // Treat the fallthrough of a branch instruction as a jump target.
            if (jump)
                infos_[fallthroughOffset].jumpTarget = true;
        }
    }

    return true;
}

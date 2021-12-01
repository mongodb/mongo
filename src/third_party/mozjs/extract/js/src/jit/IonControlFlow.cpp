/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonControlFlow.h"

#include "mozilla/DebugOnly.h"

using namespace js;
using namespace js::jit;
using mozilla::DebugOnly;

ControlFlowGenerator::ControlFlowGenerator(TempAllocator& temp, JSScript* script)
  : script(script),
    current(nullptr),
    alloc_(temp),
    blocks_(temp),
    cfgStack_(temp),
    loops_(temp),
    switches_(temp),
    labels_(temp),
    aborted_(false),
    checkedTryFinally_(false)
{ }

static inline int32_t
GetJumpOffset(jsbytecode* pc)
{
    MOZ_ASSERT(CodeSpec[JSOp(*pc)].type() == JOF_JUMP);
    return GET_JUMP_OFFSET(pc);
}

void
ControlFlowGraph::dump(GenericPrinter& print, JSScript* script)
{
    if (blocks_.length() == 0) {
        print.printf("Didn't run yet.\n");
        return;
    }

    fprintf(stderr, "Dumping cfg:\n\n");
    for (size_t i = 0; i < blocks_.length(); i++) {
        print.printf(" Block %zu, %zu:%zu\n",
                     blocks_[i].id(),
                     script->pcToOffset(blocks_[i].startPc()),
                     script->pcToOffset(blocks_[i].stopPc()));

        jsbytecode* pc = blocks_[i].startPc();
        for ( ; pc < blocks_[i].stopPc(); pc += CodeSpec[JSOp(*pc)].length) {
            MOZ_ASSERT(pc < script->codeEnd());
            print.printf("  %zu: %s\n", script->pcToOffset(pc),
                                                  CodeName[JSOp(*pc)]);
        }

        if (blocks_[i].stopIns()->isGoto()) {
            print.printf("  %s (popping:%zu) [",
                         blocks_[i].stopIns()->Name(),
                         blocks_[i].stopIns()->toGoto()->popAmount());
        } else {
            print.printf("  %s [", blocks_[i].stopIns()->Name());
        }
        for (size_t j=0; j<blocks_[i].stopIns()->numSuccessors(); j++) {
            if (j!=0)
                print.printf(", ");
            print.printf("%zu", blocks_[i].stopIns()->getSuccessor(j)->id());
        }
        print.printf("]\n\n");
    }
}

bool
ControlFlowGraph::init(TempAllocator& alloc, const CFGBlockVector& blocks)
{
    if (!blocks_.reserve(blocks.length()))
        return false;

    for (size_t i = 0; i < blocks.length(); i++) {
        MOZ_ASSERT(blocks[i]->id() == i);
        CFGBlock block(blocks[i]->startPc());

        block.setStopPc(blocks[i]->stopPc());
        block.setId(i);
        blocks_.infallibleAppend(mozilla::Move(block));
    }

    for (size_t i = 0; i < blocks.length(); i++) {
        if (!alloc.ensureBallast())
            return false;

        CFGControlInstruction* copy = nullptr;
        CFGControlInstruction* ins = blocks[i]->stopIns();
        switch (ins->type()) {
          case CFGControlInstruction::Type_Goto: {
            CFGBlock* successor = &blocks_[ins->getSuccessor(0)->id()];
            copy = CFGGoto::CopyWithNewTargets(alloc, ins->toGoto(), successor);
            break;
          }
          case CFGControlInstruction::Type_BackEdge: {
            CFGBlock* successor = &blocks_[ins->getSuccessor(0)->id()];
            copy = CFGBackEdge::CopyWithNewTargets(alloc, ins->toBackEdge(), successor);
            break;
          }
          case CFGControlInstruction::Type_LoopEntry: {
            CFGLoopEntry* old = ins->toLoopEntry();
            CFGBlock* successor = &blocks_[ins->getSuccessor(0)->id()];
            copy = CFGLoopEntry::CopyWithNewTargets(alloc, old, successor);
            break;
          }
          case CFGControlInstruction::Type_Throw: {
            copy = CFGThrow::New(alloc);
            break;
          }
          case CFGControlInstruction::Type_Test: {
            CFGTest* old = ins->toTest();
            CFGBlock* trueBranch = &blocks_[old->trueBranch()->id()];
            CFGBlock* falseBranch = &blocks_[old->falseBranch()->id()];
            copy = CFGTest::CopyWithNewTargets(alloc, old, trueBranch, falseBranch);
            break;
          }
          case CFGControlInstruction::Type_Compare: {
            CFGCompare* old = ins->toCompare();
            CFGBlock* trueBranch = &blocks_[old->trueBranch()->id()];
            CFGBlock* falseBranch = &blocks_[old->falseBranch()->id()];
            copy = CFGCompare::CopyWithNewTargets(alloc, old, trueBranch, falseBranch);
            break;
          }
          case CFGControlInstruction::Type_Return: {
            copy = CFGReturn::New(alloc);
            break;
          }
          case CFGControlInstruction::Type_RetRVal: {
            copy = CFGRetRVal::New(alloc);
            break;
          }
          case CFGControlInstruction::Type_Try: {
            CFGTry* old = ins->toTry();
            CFGBlock* tryBlock = &blocks_[old->tryBlock()->id()];
            CFGBlock* merge = nullptr;
            if (old->numSuccessors() == 2)
                merge = &blocks_[old->afterTryCatchBlock()->id()];
            copy = CFGTry::CopyWithNewTargets(alloc, old, tryBlock, merge);
            break;
          }
          case CFGControlInstruction::Type_TableSwitch: {
            CFGTableSwitch* old = ins->toTableSwitch();
            CFGTableSwitch* tableSwitch =
                CFGTableSwitch::New(alloc, old->low(), old->high());
            if (!tableSwitch->addDefault(&blocks_[old->defaultCase()->id()]))
                return false;
            for (size_t i = 0; i < ins->numSuccessors() - 1; i++) {
                if (!tableSwitch->addCase(&blocks_[old->getCase(i)->id()]))
                    return false;
            }
            copy = tableSwitch;
            break;
          }
        }
        MOZ_ASSERT(copy);
        blocks_[i].setStopIns(copy);
    }
    return true;
}

bool
ControlFlowGenerator::addBlock(CFGBlock* block)
{
    block->setId(blocks_.length());
    return blocks_.append(block);
}

// We try to build a control-flow graph in the order that it would be built as
// if traversing the AST. This leads to a nice ordering and lets us build SSA
// in one pass, since the bytecode is structured.
//
// Things get interesting when we encounter a control structure. This can be
// either an IFEQ, downward GOTO, or a decompiler hint stashed away in source
// notes. Once we encounter such an opcode, we recover the structure of the
// control flow (its branches and bounds), and push it on a stack.
//
// As we continue traversing the bytecode, we look for points that would
// terminate the topmost control flow path pushed on the stack. These are:
//  (1) The bounds of the current structure (end of a loop or join/edge of a
//      branch).
//  (2) A "return", "break", or "continue" statement.
//
// For (1), we expect that there is a current block in the progress of being
// built, and we complete the necessary edges in the CFG. For (2), we expect
// that there is no active block.
bool
ControlFlowGenerator::traverseBytecode()
{
    blocks_.clear();

    current = CFGBlock::New(alloc(), script->code());
    pc = current->startPc();

    if (!addBlock(current))
        return false;

    for (;;) {
        MOZ_ASSERT(pc < script->codeEnd());

        for (;;) {
            if (!alloc().ensureBallast())
                return false;

            // Check if we've hit an expected join point or edge in the bytecode.
            // Leaving one control structure could place us at the edge of another,
            // thus |while| instead of |if| so we don't skip any opcodes.
            MOZ_ASSERT_IF(!cfgStack_.empty(), cfgStack_.back().stopAt >= pc);
            if (!cfgStack_.empty() && cfgStack_.back().stopAt == pc) {
                ControlStatus status = processCfgStack();
                if (status == ControlStatus::Error)
                    return false;
                if (status == ControlStatus::Abort) {
                    aborted_ = true;
                    return false;
                }
                if (!current)
                    return true;
                continue;
            }

            // Some opcodes need to be handled early because they affect control
            // flow, terminating the current basic block and/or instructing the
            // traversal algorithm to continue from a new pc.
            //
            //   (1) If the opcode does not affect control flow, then the opcode
            //       is inspected and transformed to IR. This is the process_opcode
            //       label.
            //   (2) A loop could be detected via a forward GOTO. In this case,
            //       we don't want to process the GOTO, but the following
            //       instruction.
            //   (3) A RETURN, STOP, BREAK, or CONTINUE may require processing the
            //       CFG stack to terminate open branches.
            //
            // Similar to above, snooping control flow could land us at another
            // control flow point, so we iterate until it's time to inspect a real
            // opcode.
            ControlStatus status;
            if ((status = snoopControlFlow(JSOp(*pc))) == ControlStatus::None)
                break;
            if (status == ControlStatus::Error)
                return false;
            if (status == ControlStatus::Abort) {
                aborted_ = true;
                return false;
            }
            if (!current)
                return true;
        }

        JSOp op = JSOp(*pc);
        pc += CodeSpec[op].length;
    }
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::snoopControlFlow(JSOp op)
{
    switch (op) {
      case JSOP_POP:
      case JSOP_NOP: {
        jssrcnote* sn = GetSrcNote(gsn, script, pc);
        return maybeLoop(op, sn);
      }

      case JSOP_RETURN:
      case JSOP_RETRVAL:
        return processReturn(op);

      case JSOP_THROW:
        return processThrow();

      case JSOP_GOTO:
      {
        jssrcnote* sn = GetSrcNote(gsn, script, pc);
        switch (sn ? SN_TYPE(sn) : SRC_NULL) {
          case SRC_BREAK:
          case SRC_BREAK2LABEL:
            return processBreak(op, sn);

          case SRC_CONTINUE:
            return processContinue(op);

          case SRC_SWITCHBREAK:
            return processSwitchBreak(op);

          case SRC_WHILE:
          case SRC_FOR_IN:
          case SRC_FOR_OF:
            // while (cond) { }
            return processWhileOrForInLoop(sn);

          default:
            // Hard assert for now - make an error later.
            MOZ_CRASH("unknown goto case");
        }
        break;
      }

      case JSOP_TABLESWITCH: {
        jssrcnote* sn = GetSrcNote(gsn, script, pc);
        return processTableSwitch(op, sn);
      }

      case JSOP_CONDSWITCH:
        return processCondSwitch();

      case JSOP_IFNE:
        // We should never reach an IFNE, it's a stopAt point, which will
        // trigger closing the loop.
        MOZ_CRASH("we should never reach an ifne!");

      case JSOP_IFEQ:
        return processIfStart(JSOP_IFEQ);

      case JSOP_AND:
      case JSOP_OR:
        return processAndOr(op);

      case JSOP_LABEL:
        return processLabel();

      case JSOP_TRY:
        return processTry();

      case JSOP_THROWMSG:
        // Not implemented yet.
        return ControlStatus::Abort;

      default:
        break;
    }
    return ControlStatus::None;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processReturn(JSOp op)
{
    MOZ_ASSERT(op == JSOP_RETURN || op == JSOP_RETRVAL);

    CFGControlInstruction* ins;
    if (op == JSOP_RETURN)
        ins = CFGReturn::New(alloc());
    else
        ins = CFGRetRVal::New(alloc());
    endCurrentBlock(ins);

    return processControlEnd();
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processThrow()
{
    CFGThrow* ins = CFGThrow::New(alloc());
    endCurrentBlock(ins);

    return processControlEnd();
}

void
ControlFlowGenerator::endCurrentBlock(CFGControlInstruction* ins)
{
    current->setStopPc(pc);
    current->setStopIns(ins);

    // Make sure no one tries to use this block now.
    current = nullptr;
}

// Processes the top of the CFG stack. This is used from two places:
// (1) processControlEnd(), whereby a break, continue, or return may interrupt
//     an in-progress CFG structure before reaching its actual termination
//     point in the bytecode.
// (2) traverseBytecode(), whereby we reach the last instruction in a CFG
//     structure.
ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processCfgStack()
{
    ControlStatus status = processCfgEntry(cfgStack_.back());

    // If this terminated a CFG structure, act like processControlEnd() and
    // keep propagating upward.
    while (status == ControlStatus::Ended) {
        popCfgStack();
        if (cfgStack_.empty())
            return status;
        status = processCfgEntry(cfgStack_.back());
    }

    // If some join took place, the current structure is finished.
    if (status == ControlStatus::Joined)
        popCfgStack();

    return status;
}

// Given that the current control flow structure has ended forcefully,
// via a return, break, or continue (rather than joining), propagate the
// termination up. For example, a return nested 5 loops deep may terminate
// every outer loop at once, if there are no intervening conditionals:
//
// for (...) {
//   for (...) {
//     return x;
//   }
// }
//
// If |current| is nullptr when this function returns, then there is no more
// control flow to be processed.
ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processControlEnd()
{
    MOZ_ASSERT(!current);

    if (cfgStack_.empty()) {
        // If there is no more control flow to process, then this is the
        // last return in the function.
        return ControlStatus::Ended;
    }

    return processCfgStack();
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processCfgEntry(CFGState& state)
{
    switch (state.state) {
      case CFGState::IF_TRUE:
      case CFGState::IF_TRUE_EMPTY_ELSE:
        return processIfEnd(state);

      case CFGState::IF_ELSE_TRUE:
        return processIfElseTrueEnd(state);

      case CFGState::IF_ELSE_FALSE:
        return processIfElseFalseEnd(state);

      case CFGState::DO_WHILE_LOOP_BODY:
        return processDoWhileBodyEnd(state);

      case CFGState::DO_WHILE_LOOP_COND:
        return processDoWhileCondEnd(state);

      case CFGState::WHILE_LOOP_COND:
        return processWhileCondEnd(state);

      case CFGState::WHILE_LOOP_BODY:
        return processWhileBodyEnd(state);

      case CFGState::FOR_LOOP_COND:
        return processForCondEnd(state);

      case CFGState::FOR_LOOP_BODY:
        return processForBodyEnd(state);

      case CFGState::FOR_LOOP_UPDATE:
        return processForUpdateEnd(state);

      case CFGState::TABLE_SWITCH:
        return processNextTableSwitchCase(state);

      case CFGState::COND_SWITCH_CASE:
        return processCondSwitchCase(state);

      case CFGState::COND_SWITCH_BODY:
        return processCondSwitchBody(state);

      case CFGState::AND_OR:
        return processAndOrEnd(state);

      case CFGState::LABEL:
        return processLabelEnd(state);

      case CFGState::TRY:
        return processTryEnd(state);

      default:
        MOZ_CRASH("unknown cfgstate");
    }
}

void
ControlFlowGenerator::popCfgStack()
{
    if (cfgStack_.back().isLoop())
        loops_.popBack();
    if (cfgStack_.back().state == CFGState::LABEL)
        labels_.popBack();
    cfgStack_.popBack();
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processLabelEnd(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::LABEL);

    // If there are no breaks and no current, controlflow is terminated.
    if (!state.label.breaks && !current)
        return ControlStatus::Ended;

    // If there are no breaks to this label, there's nothing to do.
    if (!state.label.breaks)
        return ControlStatus::Joined;

    CFGBlock* successor = createBreakCatchBlock(state.label.breaks, state.stopAt);
    if (!successor)
        return ControlStatus::Error;

    if (current) {
        current->setStopIns(CFGGoto::New(alloc(), successor));
        current->setStopPc(pc);
    }

    current = successor;
    pc = successor->startPc();

    if (!addBlock(successor))
        return ControlStatus::Error;

    return ControlStatus::Joined;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processTry()
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_TRY);

    // Try-finally is not yet supported.
    if (!checkedTryFinally_) {
        JSTryNote* tn = script->trynotes()->vector;
        JSTryNote* tnlimit = tn + script->trynotes()->length;
        for (; tn < tnlimit; tn++) {
            if (tn->kind == JSTRY_FINALLY)
                return ControlStatus::Abort;
        }
        checkedTryFinally_ = true;
    }

    jssrcnote* sn = GetSrcNote(gsn, script, pc);
    MOZ_ASSERT(SN_TYPE(sn) == SRC_TRY);

    // Get the pc of the last instruction in the try block. It's a JSOP_GOTO to
    // jump over the catch block.
    jsbytecode* endpc = pc + GetSrcNoteOffset(sn, 0);
    MOZ_ASSERT(JSOp(*endpc) == JSOP_GOTO);
    MOZ_ASSERT(GetJumpOffset(endpc) > 0);

    jsbytecode* afterTry = endpc + GetJumpOffset(endpc);

    // If controlflow in the try body is terminated (by a return or throw
    // statement), the code after the try-statement may still be reachable
    // via the catch block (which we don't compile) and OSR can enter it.
    // For example:
    //
    //     try {
    //         throw 3;
    //     } catch(e) { }
    //
    //     for (var i=0; i<1000; i++) {}
    //
    // To handle this, we create two blocks: one for the try block and one
    // for the code following the try-catch statement.

    CFGBlock* tryBlock = CFGBlock::New(alloc(), GetNextPc(pc));

    CFGBlock* successor = CFGBlock::New(alloc(), afterTry);
    current->setStopIns(CFGTry::New(alloc(), tryBlock, endpc, successor));
    current->setStopPc(pc);

    if (!cfgStack_.append(CFGState::Try(endpc, successor)))
        return ControlStatus::Error;

    current = tryBlock;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processTryEnd(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::TRY);
    MOZ_ASSERT(state.try_.successor);

    if (current) {
        current->setStopIns(CFGGoto::New(alloc(), state.try_.successor));
        current->setStopPc(pc);
    }

    // Start parsing the code after this try-catch statement.
    current = state.try_.successor;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Joined;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processIfEnd(CFGState& state)
{
    if (current) {
        // Here, the false block is the join point. Create an edge from the
        // current block to the false block. Note that a RETURN opcode
        // could have already ended the block.
        current->setStopIns(CFGGoto::New(alloc(), state.branch.ifFalse));
        current->setStopPc(pc);
    }

    current = state.branch.ifFalse;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Joined;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processIfElseTrueEnd(CFGState& state)
{
    // We've reached the end of the true branch of an if-else. Don't
    // create an edge yet, just transition to parsing the false branch.
    state.state = CFGState::IF_ELSE_FALSE;
    state.branch.ifTrue = current;
    state.stopAt = state.branch.falseEnd;

    if (current)
        current->setStopPc(pc);

    current = state.branch.ifFalse;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processIfElseFalseEnd(CFGState& state)
{
    // Update the state to have the latest block from the false path.
    state.branch.ifFalse = current;
    if (current)
        current->setStopPc(pc);

    // To create the join node, we need an incoming edge that has not been
    // terminated yet.
    CFGBlock* pred = state.branch.ifTrue
                        ? state.branch.ifTrue
                        : state.branch.ifFalse;
    CFGBlock* other = (pred == state.branch.ifTrue)
                        ? state.branch.ifFalse
                        : state.branch.ifTrue;

    if (!pred)
        return ControlStatus::Ended;

    // Create a new block to represent the join.
    CFGBlock* join = CFGBlock::New(alloc(), state.branch.falseEnd);

    // Create edges from the true and false blocks as needed.
    pred->setStopIns(CFGGoto::New(alloc(), join));

    if (other)
        other->setStopIns(CFGGoto::New(alloc(), join));

    // Ignore unreachable remainder of false block if existent.
    current = join;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Joined;
}

CFGBlock*
ControlFlowGenerator::createBreakCatchBlock(DeferredEdge* edge, jsbytecode* pc)
{
    // Create block, using the first break statement as predecessor
    CFGBlock* successor = CFGBlock::New(alloc(), pc);

    // Finish up remaining breaks.
    while (edge) {
        if (!alloc().ensureBallast())
            return nullptr;

        CFGGoto* brk = CFGGoto::New(alloc(), successor);
        edge->block->setStopIns(brk);
        edge = edge->next;
    }

    return successor;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processDoWhileBodyEnd(CFGState& state)
{
    if (!processDeferredContinues(state))
        return ControlStatus::Error;

    // No current means control flow cannot reach the condition, so this will
    // never loop.
    if (!current)
        return processBrokenLoop(state);

    CFGBlock* header = CFGBlock::New(alloc(), state.loop.updatepc);
    current->setStopIns(CFGGoto::New(alloc(), header));
    current->setStopPc(pc);

    state.state = CFGState::DO_WHILE_LOOP_COND;
    state.stopAt = state.loop.updateEnd;

    current = header;
    pc = header->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processDoWhileCondEnd(CFGState& state)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // We're guaranteed a |current|, it's impossible to break or return from
    // inside the conditional expression.
    MOZ_ASSERT(current);

    // Create the successor block.
    CFGBlock* successor = CFGBlock::New(alloc(), GetNextPc(pc));

    CFGLoopEntry* entry = state.loop.entry->stopIns()->toLoopEntry();
    entry->setLoopStopPc(pc);

    // Create backedge with pc at start of loop to make sure we capture the
    // right stack.
    CFGBlock* backEdge = CFGBlock::New(alloc(), entry->successor()->startPc());
    backEdge->setStopIns(CFGBackEdge::New(alloc(), entry->successor()));
    backEdge->setStopPc(entry->successor()->startPc());

    if (!addBlock(backEdge))
        return ControlStatus::Error;

    // Create the test instruction and end the current block.
    CFGTest* test = CFGTest::New(alloc(), backEdge, successor);
    current->setStopIns(test);
    current->setStopPc(pc);
    return finishLoop(state, successor);
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processWhileCondEnd(CFGState& state)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_IFNE || JSOp(*pc) == JSOP_IFEQ);

    // Create the body and successor blocks.
    CFGBlock* body = CFGBlock::New(alloc(), state.loop.bodyStart);
    state.loop.successor = CFGBlock::New(alloc(), state.loop.exitpc);
    if (!body || !state.loop.successor)
        return ControlStatus::Error;

    CFGTest* test;
    if (JSOp(*pc) == JSOP_IFNE)
        test = CFGTest::New(alloc(), body, state.loop.successor);
    else
        test = CFGTest::New(alloc(), state.loop.successor, body);
    current->setStopIns(test);
    current->setStopPc(pc);

    state.state = CFGState::WHILE_LOOP_BODY;
    state.stopAt = state.loop.bodyEnd;

    current = body;
    pc = body->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processWhileBodyEnd(CFGState& state)
{
    if (!processDeferredContinues(state))
        return ControlStatus::Error;

    if (!current)
        return processBrokenLoop(state);

    CFGLoopEntry* entry = state.loop.entry->stopIns()->toLoopEntry();
    entry->setLoopStopPc(pc);

    current->setStopIns(CFGBackEdge::New(alloc(), entry->successor()));
    if (pc != current->startPc()) {
        current->setStopPc(pc);
    } else {
        // If the block is empty update the pc to the start of loop to make
        // sure we capture the right stack.
        current->setStartPc(entry->successor()->startPc());
        current->setStopPc(entry->successor()->startPc());
    }
    return finishLoop(state, state.loop.successor);
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processForCondEnd(CFGState& state)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // Create the body and successor blocks.
    CFGBlock* body = CFGBlock::New(alloc(), state.loop.bodyStart);
    state.loop.successor = CFGBlock::New(alloc(), state.loop.exitpc);

    CFGTest* test = CFGTest::New(alloc(), body, state.loop.successor);
    current->setStopIns(test);
    current->setStopPc(pc);

    state.state = CFGState::FOR_LOOP_BODY;
    state.stopAt = state.loop.bodyEnd;

    current = body;
    pc = body->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processForBodyEnd(CFGState& state)
{
    if (!processDeferredContinues(state))
        return ControlStatus::Error;

    // If there is no updatepc, just go right to processing what would be the
    // end of the update clause. Otherwise, |current| might be nullptr; if this is
    // the case, the update is unreachable anyway.
    if (!state.loop.updatepc || !current)
        return processForUpdateEnd(state);

    //MOZ_ASSERT(pc == state.loop.updatepc);

    if (state.loop.updatepc != pc) {
        CFGBlock* next = CFGBlock::New(alloc(), state.loop.updatepc);
        current->setStopIns(CFGGoto::New(alloc(), next));
        current->setStopPc(pc);
        current = next;

        if (!addBlock(current))
            return ControlStatus::Error;
    }

    pc = state.loop.updatepc;

    state.state = CFGState::FOR_LOOP_UPDATE;
    state.stopAt = state.loop.updateEnd;
    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processForUpdateEnd(CFGState& state)
{
    // If there is no current, we couldn't reach the loop edge and there was no
    // update clause.
    if (!current)
        return processBrokenLoop(state);

    CFGLoopEntry* entry = state.loop.entry->stopIns()->toLoopEntry();
    entry->setLoopStopPc(pc);

    current->setStopIns(CFGBackEdge::New(alloc(), entry->successor()));
    if (pc != current->startPc()) {
        current->setStopPc(pc);
    } else {
        // If the block is empty update the pc to the start of loop to make
        // sure we capture the right stack.
        current->setStartPc(entry->successor()->startPc());
        current->setStopPc(entry->successor()->startPc());
    }
    return finishLoop(state, state.loop.successor);
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processWhileOrForInLoop(jssrcnote* sn)
{
    // while (cond) { } loops have the following structure:
    //    GOTO cond   ; SRC_WHILE (offset to IFNE)
    //    LOOPHEAD
    //    ...
    //  cond:
    //    LOOPENTRY
    //    ...
    //    IFNE        ; goes to LOOPHEAD
    // for (x in y) { } loops are similar; the cond will be a MOREITER.
    MOZ_ASSERT(SN_TYPE(sn) == SRC_FOR_OF || SN_TYPE(sn) == SRC_FOR_IN || SN_TYPE(sn) == SRC_WHILE);
    int ifneOffset = GetSrcNoteOffset(sn, 0);
    jsbytecode* ifne = pc + ifneOffset;
    MOZ_ASSERT(ifne > pc);

    // Verify that the IFNE goes back to a loophead op.
    MOZ_ASSERT(JSOp(*GetNextPc(pc)) == JSOP_LOOPHEAD);
    MOZ_ASSERT(GetNextPc(pc) == ifne + GetJumpOffset(ifne));

    jsbytecode* loopEntry = pc + GetJumpOffset(pc);

    size_t stackPhiCount;
    if (SN_TYPE(sn) == SRC_FOR_OF)
        stackPhiCount = 3;
    else if (SN_TYPE(sn) == SRC_FOR_IN)
        stackPhiCount = 1;
    else
        stackPhiCount = 0;

    // Skip past the JSOP_LOOPHEAD for the body start.
    jsbytecode* loopHead = GetNextPc(pc);
    jsbytecode* bodyStart = GetNextPc(loopHead);
    jsbytecode* bodyEnd = pc + GetJumpOffset(pc);
    jsbytecode* exitpc = GetNextPc(ifne);
    jsbytecode* continuepc = pc;

    CFGBlock* header = CFGBlock::New(alloc(), GetNextPc(loopEntry));

    CFGLoopEntry* ins = CFGLoopEntry::New(alloc(), header, stackPhiCount);
    if (LoopEntryCanIonOsr(loopEntry))
        ins->setCanOsr();

    if (SN_TYPE(sn) == SRC_FOR_IN)
        ins->setIsForIn();

    current->setStopIns(ins);
    current->setStopPc(pc);

    if (!pushLoop(CFGState::WHILE_LOOP_COND, ifne, current,
                  loopHead, bodyEnd, bodyStart, bodyEnd, exitpc, continuepc))
    {
        return ControlStatus::Error;
    }

    // Parse the condition first.
    current = header;
    pc = header->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processBrokenLoop(CFGState& state)
{
    MOZ_ASSERT(!current);

    {
        state.loop.entry->setStopIns(
            CFGGoto::New(alloc(), state.loop.entry->stopIns()->toLoopEntry()->successor()));
    }

    // If the loop started with a condition (while/for) then even if the
    // structure never actually loops, the condition itself can still fail and
    // thus we must resume at the successor, if one exists.
    current = state.loop.successor;
    if (current) {
        if (!addBlock(current))
            return ControlStatus::Error;
    }

    // Join the breaks together and continue parsing.
    if (state.loop.breaks) {
        CFGBlock* block = createBreakCatchBlock(state.loop.breaks, state.loop.exitpc);
        if (!block)
            return ControlStatus::Error;

        if (current) {
            current->setStopIns(CFGGoto::New(alloc(), block));
            current->setStopPc(current->startPc());
        }

        current = block;

        if (!addBlock(current))
            return ControlStatus::Error;
    }

    // If the loop is not gated on a condition, and has only returns, we'll
    // reach this case. For example:
    // do { ... return; } while ();
    if (!current)
        return ControlStatus::Ended;

    // Otherwise, the loop is gated on a condition and/or has breaks so keep
    // parsing at the successor.
    pc = current->startPc();
    return ControlStatus::Joined;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::finishLoop(CFGState& state, CFGBlock* successor)
{
    MOZ_ASSERT(current);

    if (state.loop.breaks) {
        if (successor) {
            if (!addBlock(successor))
                return ControlStatus::Error;
        }

        // Create a catch block to join all break exits.
        CFGBlock* block = createBreakCatchBlock(state.loop.breaks, state.loop.exitpc);
        if (!block)
            return ControlStatus::Error;

        if (successor) {
            // Finally, create an unconditional edge from the successor to the
            // catch block.
            successor->setStopIns(CFGGoto::New(alloc(), block));
            successor->setStopPc(successor->startPc());
        }
        successor = block;
    }

    // An infinite loop (for (;;) { }) will not have a successor.
    if (!successor) {
        current = nullptr;
        return ControlStatus::Ended;
    }

    current = successor;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Joined;
}

bool
ControlFlowGenerator::processDeferredContinues(CFGState& state)
{
    // If there are any continues for this loop, and there is an update block,
    // then we need to create a new basic block to house the update.
    if (state.loop.continues) {
        DeferredEdge* edge = state.loop.continues;

        CFGBlock* update = CFGBlock::New(alloc(), pc);

        if (current) {
            current->setStopIns(CFGGoto::New(alloc(), update));
            current->setStopPc(pc);
        }

        // Remaining edges
        while (edge) {
            if (!alloc().ensureBallast())
                return false;
            edge->block->setStopIns(CFGGoto::New(alloc(), update));
            edge = edge->next;
        }
        state.loop.continues = nullptr;

        current = update;
        if (!addBlock(current))
            return false;
    }

    return true;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processCondSwitch()
{
    // CondSwitch op looks as follows:
    //   condswitch [length +exit_pc; first case offset +next-case ]
    //   {
    //     {
    //       ... any code ...
    //       case (+jump) [pcdelta offset +next-case]
    //     }+
    //     default (+jump)
    //     ... jump targets ...
    //   }
    //
    // The default case is always emitted even if there is no default case in
    // the source.  The last case statement pcdelta source note might have a 0
    // offset on the last case (not all the time).
    //
    // A conditional evaluate the condition of each case and compare it to the
    // switch value with a strict equality.  Cases conditions are iterated
    // linearly until one is matching. If one case succeeds, the flow jumps into
    // the corresponding body block.  The body block might alias others and
    // might continue in the next body block if the body is not terminated with
    // a break.
    //
    // Algorithm:
    //  1/ Loop over the case chain to reach the default target
    //   & Estimate the number of uniq bodies.
    //  2/ Generate code for all cases (see processCondSwitchCase).
    //  3/ Generate code for all bodies (see processCondSwitchBody).

    MOZ_ASSERT(JSOp(*pc) == JSOP_CONDSWITCH);
    jssrcnote* sn = GetSrcNote(gsn, script, pc);
    MOZ_ASSERT(SN_TYPE(sn) == SRC_CONDSWITCH);

    // Get the exit pc
    jsbytecode* exitpc = pc + GetSrcNoteOffset(sn, 0);
    jsbytecode* firstCase = pc + GetSrcNoteOffset(sn, 1);

    // Iterate all cases in the conditional switch.
    // - Stop at the default case. (always emitted after the last case)
    // - Estimate the number of uniq bodies. This estimation might be off by 1
    //   if the default body alias a case body.
    jsbytecode* curCase = firstCase;
    jsbytecode* lastTarget = GetJumpOffset(curCase) + curCase;
    size_t nbBodies = 1; // default target and the first body.

    MOZ_ASSERT(pc < curCase && curCase <= exitpc);
    while (JSOp(*curCase) == JSOP_CASE) {
        // Fetch the next case.
        jssrcnote* caseSn = GetSrcNote(gsn, script, curCase);
        MOZ_ASSERT(caseSn && SN_TYPE(caseSn) == SRC_NEXTCASE);
        ptrdiff_t off = GetSrcNoteOffset(caseSn, 0);
        MOZ_ASSERT_IF(off == 0, JSOp(*GetNextPc(curCase)) == JSOP_JUMPTARGET);
        curCase = off ? curCase + off : GetNextPc(GetNextPc(curCase));
        MOZ_ASSERT(pc < curCase && curCase <= exitpc);

        // Count non-aliased cases.
        jsbytecode* curTarget = GetJumpOffset(curCase) + curCase;
        if (lastTarget < curTarget)
            nbBodies++;
        lastTarget = curTarget;
    }

    // The current case now be the default case which jump to the body of the
    // default case, which might be behind the last target.
    MOZ_ASSERT(JSOp(*curCase) == JSOP_DEFAULT);
    jsbytecode* defaultTarget = GetJumpOffset(curCase) + curCase;
    MOZ_ASSERT(curCase < defaultTarget && defaultTarget <= exitpc);

    // Iterate over all cases again to find the position of the default block.
    curCase = firstCase;
    lastTarget = nullptr;
    size_t defaultIdx = 0;
    while (JSOp(*curCase) == JSOP_CASE) {
        jsbytecode* curTarget = GetJumpOffset(curCase) + curCase;
        if (lastTarget < defaultTarget && defaultTarget <= curTarget) {
            if (defaultTarget < curTarget)
                nbBodies++;
            break;
        }
        if (lastTarget < curTarget)
            defaultIdx++;

        jssrcnote* caseSn = GetSrcNote(gsn, script, curCase);
        ptrdiff_t off = GetSrcNoteOffset(caseSn, 0);
        curCase = off ? curCase + off : GetNextPc(GetNextPc(curCase));
        lastTarget = curTarget;
    }

    // Allocate the current graph state.
    CFGState state = CFGState::CondSwitch(alloc(), exitpc, defaultTarget);
    if (!state.switch_.bodies || !state.switch_.bodies->init(alloc(), nbBodies))
        return ControlStatus::Error;
    state.switch_.defaultIdx = defaultIdx;

    // Create the default case already.
    FixedList<CFGBlock*>& bodies = *state.switch_.bodies;
    bodies[state.switch_.defaultIdx] = CFGBlock::New(alloc(), defaultTarget);

    // Skip default case.
    if (state.switch_.defaultIdx == 0)
        state.switch_.currentIdx++;

    // We loop on case conditions with processCondSwitchCase.
    MOZ_ASSERT(JSOp(*firstCase) == JSOP_CASE);
    state.stopAt = firstCase;
    state.state = CFGState::COND_SWITCH_CASE;

    if (!cfgStack_.append(state))
        return ControlStatus::Error;

    jsbytecode* nextPc = GetNextPc(pc);
    CFGBlock* next = CFGBlock::New(alloc(), nextPc);

    current->setStopIns(CFGGoto::New(alloc(), next));
    current->setStopPc(pc);

    current = next;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processCondSwitchCase(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::COND_SWITCH_CASE);
    MOZ_ASSERT(!state.switch_.breaks);
    MOZ_ASSERT(current);
    MOZ_ASSERT(JSOp(*pc) == JSOP_CASE);
    FixedList<CFGBlock*>& bodies = *state.switch_.bodies;
    uint32_t& currentIdx = state.switch_.currentIdx;

    jsbytecode* lastTarget = currentIdx ? bodies[currentIdx - 1]->startPc() : nullptr;

    // Fetch the following case in which we will continue.
    jssrcnote* sn = GetSrcNote(gsn, script, pc);
    ptrdiff_t off = GetSrcNoteOffset(sn, 0);
    MOZ_ASSERT_IF(off == 0, JSOp(*GetNextPc(pc)) == JSOP_JUMPTARGET);
    jsbytecode* casePc = off ? pc + off : GetNextPc(GetNextPc(pc));
    bool nextIsDefault = JSOp(*casePc) == JSOP_DEFAULT;
    MOZ_ASSERT(JSOp(*casePc) == JSOP_CASE || nextIsDefault);

    // Allocate the block of the matching case.
    jsbytecode* bodyTarget = pc + GetJumpOffset(pc);
    CFGBlock* bodyBlock = nullptr;
    if (lastTarget < bodyTarget) {

        // Skip default case.
        if (currentIdx == state.switch_.defaultIdx) {
            currentIdx++;
            lastTarget = bodies[currentIdx - 1]->startPc();
            if (lastTarget < bodyTarget) {
                bodyBlock = CFGBlock::New(alloc(), bodyTarget);
                bodies[currentIdx++] = bodyBlock;
            } else {
                // This body alias the previous one.
                MOZ_ASSERT(lastTarget == bodyTarget);
                MOZ_ASSERT(currentIdx > 0);
                bodyBlock = bodies[currentIdx - 1];
            }
        } else {
            bodyBlock = CFGBlock::New(alloc(), bodyTarget);
            bodies[currentIdx++] = bodyBlock;
        }

    } else {
        // This body alias the previous one.
        MOZ_ASSERT(lastTarget == bodyTarget);
        MOZ_ASSERT(currentIdx > 0);
        bodyBlock = bodies[currentIdx - 1];
    }

    CFGBlock* emptyBlock = CFGBlock::New(alloc(), bodyBlock->startPc());
    emptyBlock->setStopIns(CFGGoto::New(alloc(), bodyBlock));
    emptyBlock->setStopPc(bodyBlock->startPc());
    if (!addBlock(emptyBlock))
        return ControlStatus::Error;

    if (nextIsDefault) {
        CFGBlock* defaultBlock = bodies[state.switch_.defaultIdx];

        CFGBlock* emptyBlock2 = CFGBlock::New(alloc(), defaultBlock->startPc());
        emptyBlock2->setStopIns(CFGGoto::New(alloc(), defaultBlock));
        emptyBlock2->setStopPc(defaultBlock->startPc());
        if (!addBlock(emptyBlock2))
            return ControlStatus::Error;

        current->setStopIns(CFGCompare::NewFalseBranchIsDefault(alloc(), emptyBlock, emptyBlock2));
        current->setStopPc(pc);

        return processCondSwitchDefault(state);
    }

    CFGBlock* nextBlock = CFGBlock::New(alloc(), GetNextPc(pc));
    current->setStopIns(CFGCompare::NewFalseBranchIsNextCompare(alloc(), emptyBlock, nextBlock));
    current->setStopPc(pc);

    // Continue until the case condition.
    current = nextBlock;
    pc = current->startPc();
    state.stopAt = casePc;

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processCondSwitchDefault(CFGState& state)
{
    uint32_t& currentIdx = state.switch_.currentIdx;

    // The last case condition is finished.  Loop in processCondSwitchBody,
    // with potential stops in processSwitchBreak.

#ifdef DEBUG
    // Test that we calculated the number of bodies correctly.
    FixedList<CFGBlock*>& bodies = *state.switch_.bodies;
    MOZ_ASSERT(state.switch_.currentIdx == bodies.length() ||
               state.switch_.defaultIdx + 1 == bodies.length());
#endif

    // Handle break statements in processSwitchBreak while processing
    // bodies.
    ControlFlowInfo breakInfo(cfgStack_.length() - 1, state.switch_.exitpc);
    if (!switches_.append(breakInfo))
        return ControlStatus::Error;

    // Jump into the first body.
    currentIdx = 0;
    current = nullptr;
    state.state = CFGState::COND_SWITCH_BODY;

    return processCondSwitchBody(state);
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processCondSwitchBody(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::COND_SWITCH_BODY);
    MOZ_ASSERT(pc <= state.switch_.exitpc);
    FixedList<CFGBlock*>& bodies = *state.switch_.bodies;
    uint32_t& currentIdx = state.switch_.currentIdx;

    MOZ_ASSERT(currentIdx <= bodies.length());
    if (currentIdx == bodies.length()) {
        MOZ_ASSERT_IF(current, pc == state.switch_.exitpc);
        return processSwitchEnd(state.switch_.breaks, state.switch_.exitpc);
    }

    // Get the next body
    CFGBlock* nextBody = bodies[currentIdx++];
    MOZ_ASSERT_IF(current, pc == nextBody->startPc());

    // The last body continue into the new one.
    if (current) {
        current->setStopIns(CFGGoto::New(alloc(), nextBody));
        current->setStopPc(pc);
    }

    // Continue in the next body.
    current = nextBody;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    if (currentIdx < bodies.length())
        state.stopAt = bodies[currentIdx]->startPc();
    else
        state.stopAt = state.switch_.exitpc;
    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processAndOrEnd(CFGState& state)
{
    MOZ_ASSERT(current);
    CFGBlock* lhs = state.branch.ifFalse;

    // Create a new block to represent the join.
    CFGBlock* join = CFGBlock::New(alloc(), state.stopAt);

    // End the rhs.
    current->setStopIns(CFGGoto::New(alloc(), join));
    current->setStopPc(pc);

    // End the lhs.
    lhs->setStopIns(CFGGoto::New(alloc(), join));
    lhs->setStopPc(pc);

    // Set the join path as current path.
    current = join;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Joined;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::maybeLoop(JSOp op, jssrcnote* sn)
{
    // This function looks at the opcode and source note and tries to
    // determine the structure of the loop. For some opcodes, like
    // POP/NOP which are not explicitly control flow, this source note is
    // optional. For opcodes with control flow, like GOTO, an unrecognized
    // or not-present source note is a compilation failure.
    switch (op) {
      case JSOP_POP:
        // for (init; ; update?) ...
        if (sn && SN_TYPE(sn) == SRC_FOR) {
            MOZ_CRASH("Not supported anymore?");
            return processForLoop(op, sn);
        }
        break;

      case JSOP_NOP:
        if (sn) {
            // do { } while (cond)
            if (SN_TYPE(sn) == SRC_WHILE)
                return processDoWhileLoop(sn);
            // Build a mapping such that given a basic block, whose successor
            // has a phi

            // for (; ; update?)
            if (SN_TYPE(sn) == SRC_FOR)
                return processForLoop(op, sn);
        }
        break;

      default:
        MOZ_CRASH("unexpected opcode");
    }

    return ControlStatus::None;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processForLoop(JSOp op, jssrcnote* sn)
{
    // Skip the NOP.
    MOZ_ASSERT(op == JSOP_NOP);
    pc = GetNextPc(pc);

    jsbytecode* condpc = pc + GetSrcNoteOffset(sn, 0);
    jsbytecode* updatepc = pc + GetSrcNoteOffset(sn, 1);
    jsbytecode* ifne = pc + GetSrcNoteOffset(sn, 2);
    jsbytecode* exitpc = GetNextPc(ifne);

    // for loops have the following structures:
    //
    //   NOP or POP
    //   [GOTO cond | NOP]
    //   LOOPHEAD
    // body:
    //    ; [body]
    // [increment:]
    //   [FRESHENBLOCKSCOPE, if needed by a cloned block]
    //    ; [increment]
    // [cond:]
    //   LOOPENTRY
    //   GOTO body
    //
    // If there is a condition (condpc != ifne), this acts similar to a while
    // loop otherwise, it acts like a do-while loop.
    //
    // Note that currently Ion does not compile pushblockscope/popblockscope as
    // necessary prerequisites to freshenblockscope.  So the code below doesn't
    // and needn't consider the implications of freshenblockscope.
    jsbytecode* bodyStart = pc;
    jsbytecode* bodyEnd = updatepc;
    jsbytecode* loopEntry = condpc;
    if (condpc != ifne) {
        MOZ_ASSERT(JSOp(*bodyStart) == JSOP_GOTO);
        MOZ_ASSERT(bodyStart + GetJumpOffset(bodyStart) == condpc);
        bodyStart = GetNextPc(bodyStart);
    } else {
        // No loop condition, such as for(j = 0; ; j++)
        if (op != JSOP_NOP) {
            // If the loop starts with POP, we have to skip a NOP.
            MOZ_ASSERT(JSOp(*bodyStart) == JSOP_NOP);
            bodyStart = GetNextPc(bodyStart);
        }
        loopEntry = GetNextPc(bodyStart);
    }
    jsbytecode* loopHead = bodyStart;
    MOZ_ASSERT(JSOp(*bodyStart) == JSOP_LOOPHEAD);
    MOZ_ASSERT(ifne + GetJumpOffset(ifne) == bodyStart);
    bodyStart = GetNextPc(bodyStart);

    MOZ_ASSERT(JSOp(*loopEntry) == JSOP_LOOPENTRY);

    CFGBlock* header = CFGBlock::New(alloc(), GetNextPc(loopEntry));

    CFGLoopEntry* ins = CFGLoopEntry::New(alloc(), header, 0);
    if (LoopEntryCanIonOsr(loopEntry))
        ins->setCanOsr();

    current->setStopIns(ins);
    current->setStopPc(pc);

    // If there is no condition, we immediately parse the body. Otherwise, we
    // parse the condition.
    jsbytecode* stopAt;
    CFGState::State initial;
    if (condpc != ifne) {
        pc = condpc;
        stopAt = ifne;
        initial = CFGState::FOR_LOOP_COND;
    } else {
        pc = bodyStart;
        stopAt = bodyEnd;
        initial = CFGState::FOR_LOOP_BODY;
    }

    if (!pushLoop(initial, stopAt, current,
                  loopHead, pc, bodyStart, bodyEnd, exitpc, updatepc))
    {
        return ControlStatus::Error;
    }

    CFGState& state = cfgStack_.back();
    state.loop.condpc = (condpc != ifne) ? condpc : nullptr;
    state.loop.updatepc = (updatepc != condpc) ? updatepc : nullptr;
    if (state.loop.updatepc)
        state.loop.updateEnd = condpc;

    current = header;
    if (!addBlock(current))
        return ControlStatus::Error;
    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processDoWhileLoop(jssrcnote* sn)
{
    // do { } while() loops have the following structure:
    //    NOP         ; SRC_WHILE (offset to COND)
    //    LOOPHEAD    ; SRC_WHILE (offset to IFNE)
    //    LOOPENTRY
    //    ...         ; body
    //    ...
    //    COND        ; start of condition
    //    ...
    //    IFNE ->     ; goes to LOOPHEAD
    int condition_offset = GetSrcNoteOffset(sn, 0);
    jsbytecode* conditionpc = pc + condition_offset;

    jssrcnote* sn2 = GetSrcNote(gsn, script, pc + 1);
    int offset = GetSrcNoteOffset(sn2, 0);
    jsbytecode* ifne = pc + offset + 1;
    MOZ_ASSERT(ifne > pc);

    // Verify that the IFNE goes back to a loophead op.
    jsbytecode* loopHead = GetNextPc(pc);
    MOZ_ASSERT(JSOp(*loopHead) == JSOP_LOOPHEAD);
    MOZ_ASSERT(loopHead == ifne + GetJumpOffset(ifne));

    jsbytecode* loopEntry = GetNextPc(loopHead);

    CFGBlock* header = CFGBlock::New(alloc(), GetNextPc(loopEntry));

    CFGLoopEntry* ins = CFGLoopEntry::New(alloc(), header, 0);
    if (LoopEntryCanIonOsr(loopEntry))
        ins->setCanOsr();

    current->setStopIns(ins);
    current->setStopPc(pc);

    jsbytecode* bodyEnd = conditionpc;
    jsbytecode* exitpc = GetNextPc(ifne);
    if (!pushLoop(CFGState::DO_WHILE_LOOP_BODY, conditionpc, current,
                  loopHead, loopEntry, loopEntry, bodyEnd, exitpc, conditionpc))
    {
        return ControlStatus::Error;
    }

    CFGState& state = cfgStack_.back();
    state.loop.updatepc = conditionpc;
    state.loop.updateEnd = ifne;

    current = header;
    pc = loopEntry;

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

bool
ControlFlowGenerator::pushLoop(CFGState::State initial, jsbytecode* stopAt, CFGBlock* entry,
                               jsbytecode* loopHead, jsbytecode* initialPc,
                               jsbytecode* bodyStart, jsbytecode* bodyEnd,
                               jsbytecode* exitpc, jsbytecode* continuepc)
{
    ControlFlowInfo loop(cfgStack_.length(), continuepc);
    if (!loops_.append(loop))
        return false;

    CFGState state;
    state.state = initial;
    state.stopAt = stopAt;
    state.loop.bodyStart = bodyStart;
    state.loop.bodyEnd = bodyEnd;
    state.loop.exitpc = exitpc;
    state.loop.entry = entry;
    state.loop.successor = nullptr;
    state.loop.breaks = nullptr;
    state.loop.continues = nullptr;
    state.loop.initialState = initial;
    state.loop.initialPc = initialPc;
    state.loop.initialStopAt = stopAt;
    state.loop.loopHead = loopHead;
    return cfgStack_.append(state);
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processBreak(JSOp op, jssrcnote* sn)
{
    MOZ_ASSERT(op == JSOP_GOTO);

    MOZ_ASSERT(SN_TYPE(sn) == SRC_BREAK ||
               SN_TYPE(sn) == SRC_BREAK2LABEL);

    // Find the break target.
    jsbytecode* target = pc + GetJumpOffset(pc);
    DebugOnly<bool> found = false;

    if (SN_TYPE(sn) == SRC_BREAK2LABEL) {
        for (size_t i = labels_.length() - 1; ; i--) {
            CFGState& cfg = cfgStack_[labels_[i].cfgEntry];
            MOZ_ASSERT(cfg.state == CFGState::LABEL);
            if (cfg.stopAt == target) {
                cfg.label.breaks = new(alloc()) DeferredEdge(current, cfg.label.breaks);
                found = true;
                break;
            }
            if (i == 0)
                break;
        }
    } else {
        for (size_t i = loops_.length() - 1; ; i--) {
            CFGState& cfg = cfgStack_[loops_[i].cfgEntry];
            MOZ_ASSERT(cfg.isLoop());
            if (cfg.loop.exitpc == target) {
                cfg.loop.breaks = new(alloc()) DeferredEdge(current, cfg.loop.breaks);
                found = true;
                break;
            }
            if (i == 0)
                break;
        }
    }

    current->setStopPc(pc);

    MOZ_ASSERT(found);

    current = nullptr;
    pc += CodeSpec[op].length;
    return processControlEnd();
}

static inline jsbytecode*
EffectiveContinue(jsbytecode* pc)
{
    if (JSOp(*pc) == JSOP_GOTO)
        return pc + GetJumpOffset(pc);
    return pc;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processContinue(JSOp op)
{
    MOZ_ASSERT(op == JSOP_GOTO);

    // Find the target loop.
    CFGState* found = nullptr;
    jsbytecode* target = pc + GetJumpOffset(pc);
    for (size_t i = loops_.length() - 1; ; i--) {
        // +1 to skip JSOP_JUMPTARGET.
        if (loops_[i].continuepc == target + 1 ||
            EffectiveContinue(loops_[i].continuepc) == target)
        {
            found = &cfgStack_[loops_[i].cfgEntry];
            break;
        }
        if (i == 0)
            break;
    }

    // There must always be a valid target loop structure. If not, there's
    // probably an off-by-something error in which pc we track.
    MOZ_ASSERT(found);
    CFGState& state = *found;

    state.loop.continues = new(alloc()) DeferredEdge(current, state.loop.continues);
    if (!state.loop.continues)
        return ControlStatus::Error;
    current->setStopPc(pc);

    current = nullptr;
    pc += CodeSpec[op].length;
    return processControlEnd();
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processSwitchBreak(JSOp op)
{
    MOZ_ASSERT(op == JSOP_GOTO);

    // Find the target switch.
    CFGState* found = nullptr;
    jsbytecode* target = pc + GetJumpOffset(pc);
    for (size_t i = switches_.length() - 1; ; i--) {
        if (switches_[i].continuepc == target) {
            found = &cfgStack_[switches_[i].cfgEntry];
            break;
        }
        if (i == 0)
            break;
    }

    // There must always be a valid target loop structure. If not, there's
    // probably an off-by-something error in which pc we track.
    MOZ_ASSERT(found);
    CFGState& state = *found;

    DeferredEdge** breaks = nullptr;
    switch (state.state) {
      case CFGState::TABLE_SWITCH:
        breaks = &state.switch_.breaks;
        break;
      case CFGState::COND_SWITCH_BODY:
        breaks = &state.switch_.breaks;
        break;
      default:
        MOZ_CRASH("Unexpected switch state.");
    }

    *breaks = new(alloc()) DeferredEdge(current, *breaks);

    current->setStopPc(pc);

    current = nullptr;
    pc += CodeSpec[op].length;
    return processControlEnd();
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processIfStart(JSOp op)
{
    // IFEQ always has a forward offset.
    jsbytecode* trueStart = pc + CodeSpec[op].length;
    jsbytecode* falseStart = pc + GetJumpOffset(pc);
    MOZ_ASSERT(falseStart > pc);

    // We only handle cases that emit source notes.
    jssrcnote* sn = GetSrcNote(gsn, script, pc);
    if (!sn)
        return ControlStatus::Error;

    // Create true and false branches.
    CFGBlock* ifTrue = CFGBlock::New(alloc(), trueStart);
    CFGBlock* ifFalse = CFGBlock::New(alloc(), falseStart);

    CFGTest* test = CFGTest::New(alloc(), ifTrue, ifFalse);
    current->setStopIns(test);
    current->setStopPc(pc);

    // The bytecode for if/ternary gets emitted either like this:
    //
    //    IFEQ X  ; src note (IF_ELSE, COND) points to the GOTO
    //    ...
    //    GOTO Z
    // X: ...     ; else/else if
    //    ...
    // Z:         ; join
    //
    // Or like this:
    //
    //    IFEQ X  ; src note (IF) has no offset
    //    ...
    // Z: ...     ; join
    //
    // We want to parse the bytecode as if we were parsing the AST, so for the
    // IF_ELSE/COND cases, we use the source note and follow the GOTO. For the
    // IF case, the IFEQ offset is the join point.
    switch (SN_TYPE(sn)) {
      case SRC_IF:
        if (!cfgStack_.append(CFGState::If(falseStart, test)))
            return ControlStatus::Error;
        break;

      case SRC_IF_ELSE:
      case SRC_COND:
      {
        // Infer the join point from the JSOP_GOTO[X] sitting here, then
        // assert as we much we can that this is the right GOTO.
        jsbytecode* trueEnd = pc + GetSrcNoteOffset(sn, 0);
        MOZ_ASSERT(trueEnd > pc);
        MOZ_ASSERT(trueEnd < falseStart);
        MOZ_ASSERT(JSOp(*trueEnd) == JSOP_GOTO);
        MOZ_ASSERT(!GetSrcNote(gsn, script, trueEnd));

        jsbytecode* falseEnd = trueEnd + GetJumpOffset(trueEnd);
        MOZ_ASSERT(falseEnd > trueEnd);
        MOZ_ASSERT(falseEnd >= falseStart);

        if (!cfgStack_.append(CFGState::IfElse(trueEnd, falseEnd, test)))
            return ControlStatus::Error;
        break;
      }

      default:
        MOZ_CRASH("unexpected source note type");
    }

    // Switch to parsing the true branch. Note that no PC update is needed,
    // it's the next instruction.
    current = ifTrue;
    pc = ifTrue->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

int
ControlFlowGenerator::CmpSuccessors(const void* a, const void* b)
{
    const CFGBlock* a0 = * (CFGBlock * const*)a;
    const CFGBlock* b0 = * (CFGBlock * const*)b;
    if (a0->startPc() == b0->startPc())
        return 0;

    return (a0->startPc() > b0->startPc()) ? 1 : -1;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processTableSwitch(JSOp op, jssrcnote* sn)
{
    // TableSwitch op contains the following data
    // (length between data is JUMP_OFFSET_LEN)
    //
    // 0: Offset of default case
    // 1: Lowest number in tableswitch
    // 2: Highest number in tableswitch
    // 3: Offset of case low
    // 4: Offset of case low+1
    // .: ...
    // .: Offset of case high

    MOZ_ASSERT(op == JSOP_TABLESWITCH);
    MOZ_ASSERT(SN_TYPE(sn) == SRC_TABLESWITCH);

    // Get the default and exit pc
    jsbytecode* exitpc = pc + GetSrcNoteOffset(sn, 0);
    jsbytecode* defaultpc = pc + GET_JUMP_OFFSET(pc);

    MOZ_ASSERT(defaultpc > pc && defaultpc <= exitpc);

    // Get the low and high from the tableswitch
    jsbytecode* pc2 = pc;
    pc2 += JUMP_OFFSET_LEN;
    int low = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;
    int high = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;

    // Create MIR instruction
    CFGTableSwitch* tableswitch = CFGTableSwitch::New(alloc(), low, high);

    // Create default case
    CFGBlock* defaultcase = CFGBlock::New(alloc(), defaultpc);

    if (!tableswitch->addDefault(defaultcase))
        return ControlStatus::Error;

    // Create cases
    jsbytecode* casepc = nullptr;
    for (int i = 0; i < high-low+1; i++) {
        if (!alloc().ensureBallast())
            return ControlStatus::Error;
        casepc = pc + GET_JUMP_OFFSET(pc2);

        MOZ_ASSERT(casepc >= pc && casepc <= exitpc);
        CFGBlock* caseBlock;

        if (casepc == pc) {
            // If the casepc equals the current pc, it is not a written case,
            // but a filled gap. That way we can use a tableswitch instead of
            // condswitch, even if not all numbers are consecutive.
            // In that case this block goes to the default case
            caseBlock = CFGBlock::New(alloc(), defaultpc);
            caseBlock->setStopIns(CFGGoto::New(alloc(), defaultcase));
        } else {
            // If this is an actual case (not filled gap),
            // add this block to the list that still needs to get processed.
            caseBlock = CFGBlock::New(alloc(), casepc);
        }

        if (!tableswitch->addCase(caseBlock))
            return ControlStatus::Error;

        pc2 += JUMP_OFFSET_LEN;
    }

    // Create info
    ControlFlowInfo switchinfo(cfgStack_.length(), exitpc);
    if (!switches_.append(switchinfo))
        return ControlStatus::Error;

    // Use a state to retrieve some information
    CFGState state = CFGState::TableSwitch(alloc(), exitpc);
    if (!state.switch_.bodies ||
        !state.switch_.bodies->init(alloc(), tableswitch->numSuccessors()))
    {
        return ControlStatus::Error;
    }

    FixedList<CFGBlock*>& bodies = *state.switch_.bodies;
    for (size_t i = 0; i < tableswitch->numSuccessors(); i++)
        bodies[i] = tableswitch->getSuccessor(i);

    qsort(bodies.begin(), state.switch_.bodies->length(),
          sizeof(CFGBlock*), CmpSuccessors);

    // Save the MIR instruction as last instruction of this block.
    current->setStopIns(tableswitch);
    current->setStopPc(pc);

    // If there is only one successor the block should stop at the end of the switch
    // Else it should stop at the start of the next successor
    if (bodies.length() > 1)
        state.stopAt = bodies[1]->startPc();
    else
        state.stopAt = exitpc;

    if (!cfgStack_.append(state))
        return ControlStatus::Error;

    current = bodies[0];
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processNextTableSwitchCase(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::TABLE_SWITCH);
    FixedList<CFGBlock*>& bodies = *state.switch_.bodies;

    state.switch_.currentIdx++;

    // Test if there are still unprocessed successors (cases/default)
    if (state.switch_.currentIdx >= bodies.length())
        return processSwitchEnd(state.switch_.breaks, state.switch_.exitpc);

    // Get the next successor
    CFGBlock* successor = bodies[state.switch_.currentIdx];

    // Add current block as predecessor if available.
    // This means the previous case didn't have a break statement.
    // So flow will continue in this block.
    if (current) {
        current->setStopIns(CFGGoto::New(alloc(), successor));
        current->setStopPc(pc);
    }

    // If this is the last successor the block should stop at the end of the tableswitch
    // Else it should stop at the start of the next successor
    if (state.switch_.currentIdx + 1 < bodies.length())
        state.stopAt = bodies[state.switch_.currentIdx + 1]->startPc();
    else
        state.stopAt = state.switch_.exitpc;

    current = bodies[state.switch_.currentIdx];
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processSwitchEnd(DeferredEdge* breaks, jsbytecode* exitpc)
{
    // No break statements, no current.
    // This means that control flow is cut-off from this point
    // (e.g. all cases have return statements).
    if (!breaks && !current)
        return ControlStatus::Ended;

    // Create successor block.
    // If there are breaks, create block with breaks as predecessor
    // Else create a block with current as predecessor
    CFGBlock* successor = nullptr;
    if (breaks) {
        successor = createBreakCatchBlock(breaks, exitpc);
        if (!successor)
            return ControlStatus::Error;
    } else {
        successor = CFGBlock::New(alloc(), exitpc);
    }

    // If there is current, the current block flows into this one.
    // So current is also a predecessor to this block
    if (current) {
        current->setStopIns(CFGGoto::New(alloc(), successor));
        current->setStopPc(pc);
    }

    current = successor;
    pc = successor->startPc();

    if (!addBlock(successor))
        return ControlStatus::Error;

    return ControlStatus::Joined;
}

ControlFlowGenerator::CFGState
ControlFlowGenerator::CFGState::If(jsbytecode* join, CFGTest* test)
{
    CFGState state;
    state.state = IF_TRUE;
    state.stopAt = join;
    state.branch.ifFalse = test->getSuccessor(1);
    state.branch.test = test;
    return state;
}

ControlFlowGenerator::CFGState
ControlFlowGenerator::CFGState::IfElse(jsbytecode* trueEnd, jsbytecode* falseEnd,
                                       CFGTest* test)
{
    CFGBlock* ifFalse = test->getSuccessor(1);

    CFGState state;
    // If the end of the false path is the same as the start of the
    // false path, then the "else" block is empty and we can devolve
    // this to the IF_TRUE case. We handle this here because there is
    // still an extra GOTO on the true path and we want stopAt to point
    // there, whereas the IF_TRUE case does not have the GOTO.
    state.state = (falseEnd == ifFalse->startPc())
                  ? IF_TRUE_EMPTY_ELSE
                  : IF_ELSE_TRUE;
    state.stopAt = trueEnd;
    state.branch.falseEnd = falseEnd;
    state.branch.ifFalse = ifFalse;
    state.branch.test = test;
    return state;
}

ControlFlowGenerator::CFGState
ControlFlowGenerator::CFGState::AndOr(jsbytecode* join, CFGBlock* lhs)
{
    CFGState state;
    state.state = AND_OR;
    state.stopAt = join;
    state.branch.ifFalse = lhs;
    state.branch.test = nullptr;
    return state;
}

ControlFlowGenerator::CFGState
ControlFlowGenerator::CFGState::TableSwitch(TempAllocator& alloc, jsbytecode* exitpc)
{
    CFGState state;
    state.state = TABLE_SWITCH;
    state.stopAt = exitpc;
    state.switch_.bodies = (FixedList<CFGBlock*>*)alloc.allocate(sizeof(FixedList<CFGBlock*>));
    state.switch_.currentIdx = 0;
    state.switch_.exitpc = exitpc;
    state.switch_.breaks = nullptr;
    return state;
}

ControlFlowGenerator::CFGState
ControlFlowGenerator::CFGState::CondSwitch(TempAllocator& alloc, jsbytecode* exitpc,
                                           jsbytecode* defaultTarget)
{
    CFGState state;
    state.state = COND_SWITCH_CASE;
    state.stopAt = nullptr;
    state.switch_.bodies = (FixedList<CFGBlock*>*)alloc.allocate(sizeof(FixedList<CFGBlock*>));
    state.switch_.currentIdx = 0;
    state.switch_.defaultTarget = defaultTarget;
    state.switch_.defaultIdx = uint32_t(-1);
    state.switch_.exitpc = exitpc;
    state.switch_.breaks = nullptr;
    return state;
}
ControlFlowGenerator::CFGState
ControlFlowGenerator::CFGState::Label(jsbytecode* exitpc)
{
    CFGState state;
    state.state = LABEL;
    state.stopAt = exitpc;
    state.label.breaks = nullptr;
    return state;
}

ControlFlowGenerator::CFGState
ControlFlowGenerator::CFGState::Try(jsbytecode* exitpc, CFGBlock* successor)
{
    CFGState state;
    state.state = TRY;
    state.stopAt = exitpc;
    state.try_.successor = successor;
    return state;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processAndOr(JSOp op)
{
    MOZ_ASSERT(op == JSOP_AND || op == JSOP_OR);

    jsbytecode* rhsStart = pc + CodeSpec[op].length;
    jsbytecode* joinStart = pc + GetJumpOffset(pc);
    MOZ_ASSERT(joinStart > pc);

    CFGBlock* evalLhs = CFGBlock::New(alloc(), joinStart);
    CFGBlock* evalRhs = CFGBlock::New(alloc(), rhsStart);

    CFGTest* test = (op == JSOP_AND)
                  ? CFGTest::New(alloc(), evalRhs, evalLhs)
                  : CFGTest::New(alloc(), evalLhs, evalRhs);
    test->keepCondition();
    current->setStopIns(test);
    current->setStopPc(pc);

    // Create the rhs block.
    if (!cfgStack_.append(CFGState::AndOr(joinStart, evalLhs)))
        return ControlStatus::Error;

    if (!addBlock(evalLhs))
        return ControlStatus::Error;

    current = evalRhs;
    pc = current->startPc();

    if (!addBlock(current))
        return ControlStatus::Error;

    return ControlStatus::Jumped;
}

ControlFlowGenerator::ControlStatus
ControlFlowGenerator::processLabel()
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_LABEL);

    jsbytecode* endpc = pc + GET_JUMP_OFFSET(pc);
    MOZ_ASSERT(endpc > pc);

    ControlFlowInfo label(cfgStack_.length(), endpc);
    if (!labels_.append(label))
        return ControlStatus::Error;

    if (!cfgStack_.append(CFGState::Label(endpc)))
        return ControlStatus::Error;

    return ControlStatus::None;
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/LIR.h"

#include <ctype.h>

#include "jsprf.h"

#include "jit/JitSpewer.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"

using namespace js;
using namespace js::jit;

LIRGraph::LIRGraph(MIRGraph* mir)
  : blocks_(),
    constantPool_(mir->alloc()),
    constantPoolMap_(mir->alloc()),
    safepoints_(mir->alloc()),
    nonCallSafepoints_(mir->alloc()),
    numVirtualRegisters_(0),
    numInstructions_(1), // First id is 1.
    localSlotCount_(0),
    argumentSlotCount_(0),
    entrySnapshot_(nullptr),
    mir_(*mir)
{
}

bool
LIRGraph::addConstantToPool(const Value& v, uint32_t* index)
{
    MOZ_ASSERT(constantPoolMap_.initialized());

    ConstantPoolMap::AddPtr p = constantPoolMap_.lookupForAdd(v);
    if (p) {
        *index = p->value();
        return true;
    }
    *index = constantPool_.length();
    return constantPool_.append(v) && constantPoolMap_.add(p, v, *index);
}

bool
LIRGraph::noteNeedsSafepoint(LInstruction* ins)
{
    // Instructions with safepoints must be in linear order.
    MOZ_ASSERT_IF(!safepoints_.empty(), safepoints_.back()->id() < ins->id());
    if (!ins->isCall() && !nonCallSafepoints_.append(ins))
        return false;
    return safepoints_.append(ins);
}

void
LIRGraph::dump(GenericPrinter& out)
{
    for (size_t i = 0; i < numBlocks(); i++) {
        getBlock(i)->dump(out);
        out.printf("\n");
    }
}

void
LIRGraph::dump()
{
    Fprinter out(stderr);
    dump(out);
    out.finish();
}

LBlock::LBlock(MBasicBlock* from)
  : block_(from),
    phis_(),
    entryMoveGroup_(nullptr),
    exitMoveGroup_(nullptr)
{
    from->assignLir(this);
}

bool
LBlock::init(TempAllocator& alloc)
{
    // Count the number of LPhis we'll need.
    size_t numLPhis = 0;
    for (MPhiIterator i(block_->phisBegin()), e(block_->phisEnd()); i != e; ++i) {
        MPhi* phi = *i;
        numLPhis += (phi->type() == MIRType_Value) ? BOX_PIECES : 1;
    }

    // Allocate space for the LPhis.
    if (!phis_.init(alloc, numLPhis))
        return false;

    // For each MIR phi, set up LIR phis as appropriate. We'll fill in their
    // operands on each incoming edge, and set their definitions at the start of
    // their defining block.
    size_t phiIndex = 0;
    size_t numPreds = block_->numPredecessors();
    for (MPhiIterator i(block_->phisBegin()), e(block_->phisEnd()); i != e; ++i) {
        MPhi* phi = *i;
        MOZ_ASSERT(phi->numOperands() == numPreds);

        int numPhis = (phi->type() == MIRType_Value) ? BOX_PIECES : 1;
        for (int i = 0; i < numPhis; i++) {
            LAllocation* inputs = alloc.allocateArray<LAllocation>(numPreds);
            if (!inputs)
                return false;

            void* addr = &phis_[phiIndex++];
            LPhi* lphi = new (addr) LPhi(phi, inputs);
            lphi->setBlock(this);
        }
    }
    return true;
}

const LInstruction*
LBlock::firstInstructionWithId() const
{
    for (LInstructionIterator i(instructions_.begin()); i != instructions_.end(); ++i) {
        if (i->id())
            return *i;
    }
    return 0;
}

LMoveGroup*
LBlock::getEntryMoveGroup(TempAllocator& alloc)
{
    if (entryMoveGroup_)
        return entryMoveGroup_;
    entryMoveGroup_ = LMoveGroup::New(alloc);
    insertBefore(*begin(), entryMoveGroup_);
    return entryMoveGroup_;
}

LMoveGroup*
LBlock::getExitMoveGroup(TempAllocator& alloc)
{
    if (exitMoveGroup_)
        return exitMoveGroup_;
    exitMoveGroup_ = LMoveGroup::New(alloc);
    insertBefore(*rbegin(), exitMoveGroup_);
    return exitMoveGroup_;
}

void
LBlock::dump(GenericPrinter& out)
{
    out.printf("block%u:\n", mir()->id());
    for (size_t i = 0; i < numPhis(); ++i) {
        getPhi(i)->dump(out);
        out.printf("\n");
    }
    for (LInstructionIterator iter = begin(); iter != end(); iter++) {
        iter->dump(out);
        out.printf("\n");
    }
}

void
LBlock::dump()
{
    Fprinter out(stderr);
    dump(out);
    out.finish();
}

static size_t
TotalOperandCount(LRecoverInfo* recoverInfo)
{
    size_t accum = 0;
    for (LRecoverInfo::OperandIter it(recoverInfo); !it; ++it) {
        if (!it->isRecoveredOnBailout())
            accum++;
    }
    return accum;
}

LRecoverInfo::LRecoverInfo(TempAllocator& alloc)
  : instructions_(alloc),
    recoverOffset_(INVALID_RECOVER_OFFSET)
{ }

LRecoverInfo*
LRecoverInfo::New(MIRGenerator* gen, MResumePoint* mir)
{
    LRecoverInfo* recoverInfo = new(gen->alloc()) LRecoverInfo(gen->alloc());
    if (!recoverInfo || !recoverInfo->init(mir))
        return nullptr;

    JitSpew(JitSpew_IonSnapshots, "Generating LIR recover info %p from MIR (%p)",
            (void*)recoverInfo, (void*)mir);

    return recoverInfo;
}

bool
LRecoverInfo::appendOperands(MNode* ins)
{
    for (size_t i = 0, end = ins->numOperands(); i < end; i++) {
        MDefinition* def = ins->getOperand(i);

        // As there is no cycle in the data-flow (without MPhi), checking for
        // isInWorkList implies that the definition is already in the
        // instruction vector, and not processed by a caller of the current
        // function.
        if (def->isRecoveredOnBailout() && !def->isInWorklist()) {
            if (!appendDefinition(def))
                return false;
        }
    }

    return true;
}

bool
LRecoverInfo::appendDefinition(MDefinition* def)
{
    MOZ_ASSERT(def->isRecoveredOnBailout());
    def->setInWorklist();

    if (!appendOperands(def))
        return false;
    return instructions_.append(def);
}

bool
LRecoverInfo::appendResumePoint(MResumePoint* rp)
{
    // Stores should be recovered first.
    for (auto iter(rp->storesBegin()), end(rp->storesEnd()); iter != end; ++iter) {
        if (!appendDefinition(iter->operand))
            return false;
    }

    if (rp->caller() && !appendResumePoint(rp->caller()))
        return false;

    if (!appendOperands(rp))
        return false;

    return instructions_.append(rp);
}

bool
LRecoverInfo::init(MResumePoint* rp)
{
    // Sort operations in the order in which we need to restore the stack. This
    // implies that outer frames, as well as operations needed to recover the
    // current frame, are located before the current frame. The inner-most
    // resume point should be the last element in the list.
    if (!appendResumePoint(rp))
        return false;

    // Remove temporary flags from all definitions.
    for (MNode** it = begin(); it != end(); it++) {
        if (!(*it)->isDefinition())
            continue;

        (*it)->toDefinition()->setNotInWorklist();
    }

    MOZ_ASSERT(mir() == rp);
    return true;
}

LSnapshot::LSnapshot(LRecoverInfo* recoverInfo, BailoutKind kind)
  : numSlots_(TotalOperandCount(recoverInfo) * BOX_PIECES),
    slots_(nullptr),
    recoverInfo_(recoverInfo),
    snapshotOffset_(INVALID_SNAPSHOT_OFFSET),
    bailoutId_(INVALID_BAILOUT_ID),
    bailoutKind_(kind)
{ }

bool
LSnapshot::init(MIRGenerator* gen)
{
    slots_ = gen->allocate<LAllocation>(numSlots_);
    return !!slots_;
}

LSnapshot*
LSnapshot::New(MIRGenerator* gen, LRecoverInfo* recover, BailoutKind kind)
{
    LSnapshot* snapshot = new(gen->alloc()) LSnapshot(recover, kind);
    if (!snapshot || !snapshot->init(gen))
        return nullptr;

    JitSpew(JitSpew_IonSnapshots, "Generating LIR snapshot %p from recover (%p)",
            (void*)snapshot, (void*)recover);

    return snapshot;
}

void
LSnapshot::rewriteRecoveredInput(LUse input)
{
    // Mark any operands to this snapshot with the same value as input as being
    // equal to the instruction's result.
    for (size_t i = 0; i < numEntries(); i++) {
        if (getEntry(i)->isUse() && getEntry(i)->toUse()->virtualRegister() == input.virtualRegister())
            setEntry(i, LUse(input.virtualRegister(), LUse::RECOVERED_INPUT));
    }
}

void
LNode::printName(GenericPrinter& out, Opcode op)
{
    static const char * const names[] =
    {
#define LIROP(x) #x,
        LIR_OPCODE_LIST(LIROP)
#undef LIROP
    };
    const char* name = names[op];
    size_t len = strlen(name);
    for (size_t i = 0; i < len; i++)
        out.printf("%c", tolower(name[i]));
}

void
LNode::printName(GenericPrinter& out)
{
    printName(out, op());
}

bool
LAllocation::aliases(const LAllocation& other) const
{
    if (isFloatReg() && other.isFloatReg())
        return toFloatReg()->reg().aliases(other.toFloatReg()->reg());
    return *this == other;
}

static const char * const TypeChars[] =
{
    "g",            // GENERAL
    "i",            // INT32
    "o",            // OBJECT
    "s",            // SLOTS
    "f",            // FLOAT32
    "d",            // DOUBLE
    "i32x4",        // INT32X4
    "f32x4",        // FLOAT32X4
    "sincos",       // SINCOS
#ifdef JS_NUNBOX32
    "t",            // TYPE
    "p"             // PAYLOAD
#elif JS_PUNBOX64
    "x"             // BOX
#endif
};

static void
PrintDefinition(char* buf, size_t size, const LDefinition& def)
{
    char* cursor = buf;
    char* end = buf + size;

    cursor += JS_snprintf(cursor, end - cursor, "v%u", def.virtualRegister());
    cursor += JS_snprintf(cursor, end - cursor, "<%s>", TypeChars[def.type()]);

    if (def.policy() == LDefinition::FIXED)
        cursor += JS_snprintf(cursor, end - cursor, ":%s", def.output()->toString());
    else if (def.policy() == LDefinition::MUST_REUSE_INPUT)
        cursor += JS_snprintf(cursor, end - cursor, ":tied(%u)", def.getReusedInput());
}

const char*
LDefinition::toString() const
{
    // Not reentrant!
    static char buf[40];

    if (isBogusTemp())
        return "bogus";

    PrintDefinition(buf, sizeof(buf), *this);
    return buf;
}

static void
PrintUse(char* buf, size_t size, const LUse* use)
{
    switch (use->policy()) {
      case LUse::REGISTER:
        JS_snprintf(buf, size, "v%d:r", use->virtualRegister());
        break;
      case LUse::FIXED:
        JS_snprintf(buf, size, "v%d:%s", use->virtualRegister(),
                    AnyRegister::FromCode(use->registerCode()).name());
        break;
      case LUse::ANY:
        JS_snprintf(buf, size, "v%d:r?", use->virtualRegister());
        break;
      case LUse::KEEPALIVE:
        JS_snprintf(buf, size, "v%d:*", use->virtualRegister());
        break;
      case LUse::RECOVERED_INPUT:
        JS_snprintf(buf, size, "v%d:**", use->virtualRegister());
        break;
      default:
        MOZ_CRASH("invalid use policy");
    }
}

const char*
LAllocation::toString() const
{
    // Not reentrant!
    static char buf[40];

    if (isBogus())
        return "bogus";

    switch (kind()) {
      case LAllocation::CONSTANT_VALUE:
      case LAllocation::CONSTANT_INDEX:
        return "c";
      case LAllocation::GPR:
        JS_snprintf(buf, sizeof(buf), "%s", toGeneralReg()->reg().name());
        return buf;
      case LAllocation::FPU:
        JS_snprintf(buf, sizeof(buf), "%s", toFloatReg()->reg().name());
        return buf;
      case LAllocation::STACK_SLOT:
        JS_snprintf(buf, sizeof(buf), "stack:%d", toStackSlot()->slot());
        return buf;
      case LAllocation::ARGUMENT_SLOT:
        JS_snprintf(buf, sizeof(buf), "arg:%d", toArgument()->index());
        return buf;
      case LAllocation::USE:
        PrintUse(buf, sizeof(buf), toUse());
        return buf;
      default:
        MOZ_CRASH("what?");
    }
}

void
LAllocation::dump() const
{
    fprintf(stderr, "%s\n", toString());
}

void
LDefinition::dump() const
{
    fprintf(stderr, "%s\n", toString());
}

void
LNode::printOperands(GenericPrinter& out)
{
    for (size_t i = 0, e = numOperands(); i < e; i++) {
        out.printf(" (%s)", getOperand(i)->toString());
        if (i != numOperands() - 1)
            out.printf(",");
    }
}

void
LInstruction::assignSnapshot(LSnapshot* snapshot)
{
    MOZ_ASSERT(!snapshot_);
    snapshot_ = snapshot;

#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_IonSnapshots)) {
        JitSpewHeader(JitSpew_IonSnapshots);
        Fprinter& out = JitSpewPrinter();
        out.printf("Assigning snapshot %p to instruction %p (",
                   (void*)snapshot, (void*)this);
        printName(out);
        out.printf(")\n");
    }
#endif
}

void
LNode::dump(GenericPrinter& out)
{
    if (numDefs() != 0) {
        out.printf("{");
        for (size_t i = 0; i < numDefs(); i++) {
            out.printf("%s", getDef(i)->toString());
            if (i != numDefs() - 1)
                out.printf(", ");
        }
        out.printf("} <- ");
    }

    printName(out);
    printOperands(out);

    if (numTemps()) {
        out.printf(" t=(");
        for (size_t i = 0; i < numTemps(); i++) {
            out.printf("%s", getTemp(i)->toString());
            if (i != numTemps() - 1)
                out.printf(", ");
        }
        out.printf(")");
    }

    if (numSuccessors()) {
        out.printf(" s=(");
        for (size_t i = 0; i < numSuccessors(); i++) {
            out.printf("block%u", getSuccessor(i)->id());
            if (i != numSuccessors() - 1)
                out.printf(", ");
        }
        out.printf(")");
    }
}

void
LNode::dump()
{
    Fprinter out(stderr);
    dump(out);
    out.printf("\n");
    out.finish();
}

void
LInstruction::initSafepoint(TempAllocator& alloc)
{
    MOZ_ASSERT(!safepoint_);
    safepoint_ = new(alloc) LSafepoint(alloc);
    MOZ_ASSERT(safepoint_);
}

bool
LMoveGroup::add(LAllocation from, LAllocation to, LDefinition::Type type)
{
#ifdef DEBUG
    MOZ_ASSERT(from != to);
    for (size_t i = 0; i < moves_.length(); i++)
        MOZ_ASSERT(to != moves_[i].to());

    // Check that SIMD moves are aligned according to ABI requirements.
    if (LDefinition(type).isSimdType()) {
        MOZ_ASSERT(from.isMemory() || from.isFloatReg());
        if (from.isMemory()) {
            if (from.isArgument())
                MOZ_ASSERT(from.toArgument()->index() % SimdMemoryAlignment == 0);
            else
                MOZ_ASSERT(from.toStackSlot()->slot() % SimdMemoryAlignment == 0);
        }
        MOZ_ASSERT(to.isMemory() || to.isFloatReg());
        if (to.isMemory()) {
            if (to.isArgument())
                MOZ_ASSERT(to.toArgument()->index() % SimdMemoryAlignment == 0);
            else
                MOZ_ASSERT(to.toStackSlot()->slot() % SimdMemoryAlignment == 0);
        }
    }
#endif
    return moves_.append(LMove(from, to, type));
}

bool
LMoveGroup::addAfter(LAllocation from, LAllocation to, LDefinition::Type type)
{
    // Transform the operands to this move so that performing the result
    // simultaneously with existing moves in the group will have the same
    // effect as if the original move took place after the existing moves.

    for (size_t i = 0; i < moves_.length(); i++) {
        if (moves_[i].to() == from) {
            from = moves_[i].from();
            break;
        }
    }

    if (from == to)
        return true;

    for (size_t i = 0; i < moves_.length(); i++) {
        if (to == moves_[i].to()) {
            moves_[i] = LMove(from, to, type);
            return true;
        }
    }

    return add(from, to, type);
}

void
LMoveGroup::printOperands(GenericPrinter& out)
{
    for (size_t i = 0; i < numMoves(); i++) {
        const LMove& move = getMove(i);
        // Use two printfs, as LAllocation::toString is not reentrant.
        out.printf(" [%s", move.from().toString());
        out.printf(" -> %s", move.to().toString());
#ifdef DEBUG
        out.printf(", %s", TypeChars[move.type()]);
#endif
        out.printf("]");
        if (i != numMoves() - 1)
            out.printf(",");
    }
}

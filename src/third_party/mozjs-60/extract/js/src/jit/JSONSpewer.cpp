/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef JS_JITSPEW

#include "jit/JSONSpewer.h"


#include <stdarg.h>

#include "jit/BacktrackingAllocator.h"
#include "jit/LIR.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "jit/RangeAnalysis.h"

using namespace js;
using namespace js::jit;

void
JSONSpewer::beginFunction(JSScript* script)
{
    beginObject();
    if (script)
        formatProperty("name", "%s:%zu", script->filename(), script->lineno());
    else
        property("name", "wasm compilation");
    beginListProperty("passes");
}

void
JSONSpewer::beginPass(const char* pass)
{
    beginObject();
    property("name", pass);
}

void
JSONSpewer::spewMResumePoint(MResumePoint* rp)
{
    if (!rp)
        return;

    beginObjectProperty("resumePoint");

    if (rp->caller())
        property("caller", rp->caller()->block()->id());

    switch (rp->mode()) {
      case MResumePoint::ResumeAt:
        property("mode", "At");
        break;
      case MResumePoint::ResumeAfter:
        property("mode", "After");
        break;
      case MResumePoint::Outer:
        property("mode", "Outer");
        break;
    }

    beginListProperty("operands");
    for (MResumePoint* iter = rp; iter; iter = iter->caller()) {
        for (int i = iter->numOperands() - 1; i >= 0; i--)
            value(iter->getOperand(i)->id());
        if (iter->caller())
            value("|");
    }
    endList();

    endObject();
}

void
JSONSpewer::spewMDef(MDefinition* def)
{
    beginObject();

    property("id", def->id());

    propertyName("opcode");
    out_.printf("\"");
    def->printOpcode(out_);
    out_.printf("\"");

    beginListProperty("attributes");
#define OUTPUT_ATTRIBUTE(X) do{ if(def->is##X()) value(#X); } while(0);
    MIR_FLAG_LIST(OUTPUT_ATTRIBUTE);
#undef OUTPUT_ATTRIBUTE
    endList();

    beginListProperty("inputs");
    for (size_t i = 0, e = def->numOperands(); i < e; i++)
        value(def->getOperand(i)->id());
    endList();

    beginListProperty("uses");
    for (MUseDefIterator use(def); use; use++)
        value(use.def()->id());
    endList();

    if (!def->isLowered()) {
        beginListProperty("memInputs");
        if (def->dependency())
            value(def->dependency()->id());
        endList();
    }

    bool isTruncated = false;
    if (def->isAdd() || def->isSub() || def->isMod() || def->isMul() || def->isDiv())
        isTruncated = static_cast<MBinaryArithInstruction*>(def)->isTruncated();

    if (def->type() != MIRType::None && def->range()) {
        beginStringProperty("type");
        def->range()->dump(out_);
        out_.printf(" : %s%s", StringFromMIRType(def->type()), (isTruncated ? " (t)" : ""));
        endStringProperty();
    } else {
        formatProperty("type", "%s%s", StringFromMIRType(def->type()), (isTruncated ? " (t)" : ""));
    }

    if (def->isInstruction()) {
        if (MResumePoint* rp = def->toInstruction()->resumePoint())
            spewMResumePoint(rp);
    }

    endObject();
}

void
JSONSpewer::spewMIR(MIRGraph* mir)
{
    beginObjectProperty("mir");
    beginListProperty("blocks");

    for (MBasicBlockIterator block(mir->begin()); block != mir->end(); block++) {
        beginObject();

        property("number", block->id());
        if (block->getHitState() == MBasicBlock::HitState::Count)
            property("count", block->getHitCount());

        beginListProperty("attributes");
        if (block->hasLastIns()) {
            if (block->isLoopBackedge())
                value("backedge");
            if (block->isLoopHeader())
                value("loopheader");
            if (block->isSplitEdge())
                value("splitedge");
        }
        endList();

        beginListProperty("predecessors");
        for (size_t i = 0; i < block->numPredecessors(); i++)
            value(block->getPredecessor(i)->id());
        endList();

        beginListProperty("successors");
        if (block->hasLastIns()) {
            for (size_t i = 0; i < block->numSuccessors(); i++)
                value(block->getSuccessor(i)->id());
        }
        endList();

        beginListProperty("instructions");
        for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++)
            spewMDef(*phi);
        for (MInstructionIterator i(block->begin()); i != block->end(); i++)
            spewMDef(*i);
        endList();

        spewMResumePoint(block->entryResumePoint());

        endObject();
    }

    endList();
    endObject();
}

void
JSONSpewer::spewLIns(LNode* ins)
{
    beginObject();

    property("id", ins->id());

    propertyName("opcode");
    out_.printf("\"");
    ins->dump(out_);
    out_.printf("\"");

    beginListProperty("defs");
    for (size_t i = 0; i < ins->numDefs(); i++) {
        if (ins->isPhi())
            value(ins->toPhi()->getDef(i)->virtualRegister());
        else
            value(ins->toInstruction()->getDef(i)->virtualRegister());
    }
    endList();

    endObject();
}

void
JSONSpewer::spewLIR(MIRGraph* mir)
{
    beginObjectProperty("lir");
    beginListProperty("blocks");

    for (MBasicBlockIterator i(mir->begin()); i != mir->end(); i++) {
        LBlock* block = i->lir();
        if (!block)
            continue;

        beginObject();
        property("number", i->id());

        beginListProperty("instructions");
        for (size_t p = 0; p < block->numPhis(); p++)
            spewLIns(block->getPhi(p));
        for (LInstructionIterator ins(block->begin()); ins != block->end(); ins++)
            spewLIns(*ins);
        endList();

        endObject();
    }

    endList();
    endObject();
}

void
JSONSpewer::spewRanges(BacktrackingAllocator* regalloc)
{
    beginObjectProperty("ranges");
    beginListProperty("blocks");

    for (size_t bno = 0; bno < regalloc->graph.numBlocks(); bno++) {
        beginObject();
        property("number", bno);
        beginListProperty("vregs");

        LBlock* lir = regalloc->graph.getBlock(bno);
        for (LInstructionIterator ins = lir->begin(); ins != lir->end(); ins++) {
            for (size_t k = 0; k < ins->numDefs(); k++) {
                uint32_t id = ins->getDef(k)->virtualRegister();
                VirtualRegister* vreg = &regalloc->vregs[id];

                beginObject();
                property("vreg", id);
                beginListProperty("ranges");

                for (LiveRange::RegisterLinkIterator iter = vreg->rangesBegin(); iter; iter++) {
                    LiveRange* range = LiveRange::get(*iter);

                    beginObject();
                    property("allocation", range->bundle()->allocation().toString().get());
                    property("start", range->from().bits());
                    property("end", range->to().bits());
                    endObject();
                }

                endList();
                endObject();
            }
        }

        endList();
        endObject();
    }

    endList();
    endObject();
}

void
JSONSpewer::endPass()
{
    endObject();
}

void
JSONSpewer::endFunction()
{
    endList();
    endObject();
}

#endif /* JS_JITSPEW */

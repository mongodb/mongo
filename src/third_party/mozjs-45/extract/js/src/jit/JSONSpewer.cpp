/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

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
JSONSpewer::indent()
{
    MOZ_ASSERT(indentLevel_ >= 0);
    out_.printf("\n");
    for (int i = 0; i < indentLevel_; i++)
        out_.printf("  ");
}

void
JSONSpewer::property(const char* name)
{
    if (!first_)
        out_.printf(",");
    indent();
    out_.printf("\"%s\":", name);
    first_ = false;
}

void
JSONSpewer::beginObject()
{
    if (!first_) {
        out_.printf(",");
        indent();
    }
    out_.printf("{");
    indentLevel_++;
    first_ = true;
}

void
JSONSpewer::beginObjectProperty(const char* name)
{
    property(name);
    out_.printf("{");
    indentLevel_++;
    first_ = true;
}

void
JSONSpewer::beginListProperty(const char* name)
{
    property(name);
    out_.printf("[");
    first_ = true;
}

void
JSONSpewer::beginStringProperty(const char* name)
{
    property(name);
    out_.printf("\"");
}

void
JSONSpewer::endStringProperty()
{
    out_.printf("\"");
}

void
JSONSpewer::stringProperty(const char* name, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);

    beginStringProperty(name);
    out_.vprintf(format, ap);
    endStringProperty();

    va_end(ap);
}

void
JSONSpewer::stringValue(const char* format, ...)
{
    va_list ap;
    va_start(ap, format);

    if (!first_)
        out_.printf(",");
    out_.printf("\"");
    out_.vprintf(format, ap);
    out_.printf("\"");

    va_end(ap);
    first_ = false;
}

void
JSONSpewer::integerProperty(const char* name, int value)
{
    property(name);
    out_.printf("%d", value);
}

void
JSONSpewer::integerValue(int value)
{
    if (!first_)
        out_.printf(",");
    out_.printf("%d", value);
    first_ = false;
}

void
JSONSpewer::endObject()
{
    indentLevel_--;
    indent();
    out_.printf("}");
    first_ = false;
}

void
JSONSpewer::endList()
{
    out_.printf("]");
    first_ = false;
}

void
JSONSpewer::beginFunction(JSScript* script)
{
    beginObject();
    if (script)
        stringProperty("name", "%s:%d", script->filename(), script->lineno());
    else
        stringProperty("name", "asm.js compilation");
    beginListProperty("passes");
}

void
JSONSpewer::beginPass(const char* pass)
{
    beginObject();
    stringProperty("name", pass);
}

void
JSONSpewer::spewMResumePoint(MResumePoint* rp)
{
    if (!rp)
        return;

    beginObjectProperty("resumePoint");

    if (rp->caller())
        integerProperty("caller", rp->caller()->block()->id());

    property("mode");
    switch (rp->mode()) {
      case MResumePoint::ResumeAt:
        out_.printf("\"At\"");
        break;
      case MResumePoint::ResumeAfter:
        out_.printf("\"After\"");
        break;
      case MResumePoint::Outer:
        out_.printf("\"Outer\"");
        break;
    }

    beginListProperty("operands");
    for (MResumePoint* iter = rp; iter; iter = iter->caller()) {
        for (int i = iter->numOperands() - 1; i >= 0; i--)
            integerValue(iter->getOperand(i)->id());
        if (iter->caller())
            stringValue("|");
    }
    endList();

    endObject();
}

void
JSONSpewer::spewMDef(MDefinition* def)
{
    beginObject();

    integerProperty("id", def->id());

    property("opcode");
    out_.printf("\"");
    def->printOpcode(out_);
    out_.printf("\"");

    beginListProperty("attributes");
#define OUTPUT_ATTRIBUTE(X) do{ if(def->is##X()) stringValue(#X); } while(0);
    MIR_FLAG_LIST(OUTPUT_ATTRIBUTE);
#undef OUTPUT_ATTRIBUTE
    endList();

    beginListProperty("inputs");
    for (size_t i = 0, e = def->numOperands(); i < e; i++)
        integerValue(def->getOperand(i)->id());
    endList();

    beginListProperty("uses");
    for (MUseDefIterator use(def); use; use++)
        integerValue(use.def()->id());
    endList();

    if (!def->isLowered()) {
        beginListProperty("memInputs");
        if (def->dependency())
            integerValue(def->dependency()->id());
        endList();
    }

    bool isTruncated = false;
    if (def->isAdd() || def->isSub() || def->isMod() || def->isMul() || def->isDiv())
        isTruncated = static_cast<MBinaryArithInstruction*>(def)->isTruncated();

    if (def->type() != MIRType_None && def->range()) {
        beginStringProperty("type");
        def->range()->dump(out_);
        out_.printf(" : %s%s", StringFromMIRType(def->type()), (isTruncated ? " (t)" : ""));
        endStringProperty();
    } else {
        stringProperty("type", "%s%s", StringFromMIRType(def->type()), (isTruncated ? " (t)" : ""));
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

        integerProperty("number", block->id());
        if (block->getHitState() == MBasicBlock::HitState::Count)
            integerProperty("count", block->getHitCount());

        beginListProperty("attributes");
        if (block->isLoopBackedge())
            stringValue("backedge");
        if (block->isLoopHeader())
            stringValue("loopheader");
        if (block->isSplitEdge())
            stringValue("splitedge");
        endList();

        beginListProperty("predecessors");
        for (size_t i = 0; i < block->numPredecessors(); i++)
            integerValue(block->getPredecessor(i)->id());
        endList();

        beginListProperty("successors");
        for (size_t i = 0; i < block->numSuccessors(); i++)
            integerValue(block->getSuccessor(i)->id());
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

    integerProperty("id", ins->id());

    property("opcode");
    out_.printf("\"");
    ins->dump(out_);
    out_.printf("\"");

    beginListProperty("defs");
    for (size_t i = 0; i < ins->numDefs(); i++)
        integerValue(ins->getDef(i)->virtualRegister());
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
        integerProperty("number", i->id());

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
        integerProperty("number", bno);
        beginListProperty("vregs");

        LBlock* lir = regalloc->graph.getBlock(bno);
        for (LInstructionIterator ins = lir->begin(); ins != lir->end(); ins++) {
            for (size_t k = 0; k < ins->numDefs(); k++) {
                uint32_t id = ins->getDef(k)->virtualRegister();
                VirtualRegister* vreg = &regalloc->vregs[id];

                beginObject();
                integerProperty("vreg", id);
                beginListProperty("ranges");

                for (LiveRange::RegisterLinkIterator iter = vreg->rangesBegin(); iter; iter++) {
                    LiveRange* range = LiveRange::get(*iter);

                    beginObject();
                    property("allocation");
                    out_.printf("\"%s\"", range->bundle()->allocation().toString());
                    integerProperty("start", range->from().bits());
                    integerProperty("end", range->to().bits());
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

void
JSONSpewer::spewDebuggerGraph(MIRGraph* graph)
{
    beginObject();
    spewMIR(graph);
    spewLIR(graph);
    endObject();
}

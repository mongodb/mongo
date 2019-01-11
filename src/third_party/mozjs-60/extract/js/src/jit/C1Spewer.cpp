/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef JS_JITSPEW

#include "jit/C1Spewer.h"


#include <time.h>

#include "jit/BacktrackingAllocator.h"
#include "jit/LIR.h"
#include "jit/MIRGraph.h"

#include "vm/Printer.h"

using namespace js;
using namespace js::jit;

void
C1Spewer::beginFunction(MIRGraph* graph, JSScript* script)
{
    this->graph  = graph;

    out_.printf("begin_compilation\n");
    if (script) {
        out_.printf("  name \"%s:%zu\"\n", script->filename(), script->lineno());
        out_.printf("  method \"%s:%zu\"\n", script->filename(), script->lineno());
    } else {
        out_.printf("  name \"wasm compilation\"\n");
        out_.printf("  method \"wasm compilation\"\n");
    }
    out_.printf("  date %d\n", (int)time(nullptr));
    out_.printf("end_compilation\n");
}

void
C1Spewer::spewPass(const char* pass)
{
    out_.printf("begin_cfg\n");
    out_.printf("  name \"%s\"\n", pass);

    for (MBasicBlockIterator block(graph->begin()); block != graph->end(); block++)
        spewPass(out_, *block);

    out_.printf("end_cfg\n");
}

void
C1Spewer::spewRanges(const char* pass, BacktrackingAllocator* regalloc)
{
    out_.printf("begin_ranges\n");
    out_.printf(" name \"%s\"\n", pass);

    for (MBasicBlockIterator block(graph->begin()); block != graph->end(); block++)
        spewRanges(out_, *block, regalloc);

    out_.printf("end_ranges\n");
}

void
C1Spewer::endFunction()
{
}

static void
DumpDefinition(GenericPrinter& out, MDefinition* def)
{
    out.printf("      ");
    out.printf("%u %u ", def->id(), unsigned(def->useCount()));
    def->printName(out);
    out.printf(" ");
    def->printOpcode(out);
    out.printf(" <|@\n");
}

static void
DumpLIR(GenericPrinter& out, LNode* ins)
{
    out.printf("      ");
    out.printf("%d ", ins->id());
    ins->dump(out);
    out.printf(" <|@\n");
}

void
C1Spewer::spewRanges(GenericPrinter& out, BacktrackingAllocator* regalloc, LNode* ins)
{
    for (size_t k = 0; k < ins->numDefs(); k++) {
        const LDefinition* def = ins->isPhi()
            ? ins->toPhi()->getDef(k)
            : ins->toInstruction()->getDef(k);
        uint32_t id = def->virtualRegister();
        VirtualRegister* vreg = &regalloc->vregs[id];

        for (LiveRange::RegisterLinkIterator iter = vreg->rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);
            out.printf("%d object \"", id);
            out.printf("%s", range->bundle()->allocation().toString().get());
            out.printf("\" %d -1", id);
            out.printf(" [%u, %u[", range->from().bits(), range->to().bits());
            for (UsePositionIterator usePos(range->usesBegin()); usePos; usePos++)
                out.printf(" %u M", usePos->pos.bits());
            out.printf(" \"\"\n");
        }
    }
}

void
C1Spewer::spewRanges(GenericPrinter& out, MBasicBlock* block, BacktrackingAllocator* regalloc)
{
    LBlock* lir = block->lir();
    if (!lir)
        return;

    for (size_t i = 0; i < lir->numPhis(); i++)
        spewRanges(out, regalloc, lir->getPhi(i));

    for (LInstructionIterator ins = lir->begin(); ins != lir->end(); ins++)
        spewRanges(out, regalloc, *ins);
}

void
C1Spewer::spewPass(GenericPrinter& out, MBasicBlock* block)
{
    out.printf("  begin_block\n");
    out.printf("    name \"B%d\"\n", block->id());
    out.printf("    from_bci -1\n");
    out.printf("    to_bci -1\n");

    out.printf("    predecessors");
    for (uint32_t i = 0; i < block->numPredecessors(); i++) {
        MBasicBlock* pred = block->getPredecessor(i);
        out.printf(" \"B%d\"", pred->id());
    }
    out.printf("\n");

    out.printf("    successors");
    if (block->hasLastIns()) {
        for (uint32_t i = 0; i < block->numSuccessors(); i++) {
            MBasicBlock* successor = block->getSuccessor(i);
            out.printf(" \"B%d\"", successor->id());
        }
    }
    out.printf("\n");

    out.printf("    xhandlers\n");
    out.printf("    flags\n");

    if (block->lir() && block->lir()->begin() != block->lir()->end()) {
        out.printf("    first_lir_id %d\n", block->lir()->firstId());
        out.printf("    last_lir_id %d\n", block->lir()->lastId());
    }

    out.printf("    begin_states\n");

    if (block->entryResumePoint()) {
        out.printf("      begin_locals\n");
        out.printf("        size %d\n", (int)block->numEntrySlots());
        out.printf("        method \"None\"\n");
        for (uint32_t i = 0; i < block->numEntrySlots(); i++) {
            MDefinition* ins = block->getEntrySlot(i);
            out.printf("        ");
            out.printf("%d ", i);
            if (ins->isUnused())
                out.printf("unused");
            else
                ins->printName(out);
            out.printf("\n");
        }
        out.printf("      end_locals\n");
    }
    out.printf("    end_states\n");

    out.printf("    begin_HIR\n");
    for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++)
        DumpDefinition(out, *phi);
    for (MInstructionIterator i(block->begin()); i != block->end(); i++)
        DumpDefinition(out, *i);
    out.printf("    end_HIR\n");

    if (block->lir()) {
        out.printf("    begin_LIR\n");
        for (size_t i = 0; i < block->lir()->numPhis(); i++)
            DumpLIR(out, block->lir()->getPhi(i));
        for (LInstructionIterator i(block->lir()->begin()); i != block->lir()->end(); i++)
            DumpLIR(out, *i);
        out.printf("    end_LIR\n");
    }

    out.printf("  end_block\n");
}

#endif /* JS_JITSPEW */


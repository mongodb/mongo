/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef DEBUG

#include "jit/C1Spewer.h"

#include <time.h>

#include "jit/LinearScan.h"
#include "jit/LIR.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace js::jit;

bool
C1Spewer::init(const char* path)
{
    spewout_ = fopen(path, "w");
    return spewout_ != nullptr;
}

void
C1Spewer::beginFunction(MIRGraph* graph, HandleScript script)
{
    if (!spewout_)
        return;

    this->graph  = graph;

    fprintf(spewout_, "begin_compilation\n");
    if (script) {
        fprintf(spewout_, "  name \"%s:%d\"\n", script->filename(), (int)script->lineno());
        fprintf(spewout_, "  method \"%s:%d\"\n", script->filename(), (int)script->lineno());
    } else {
        fprintf(spewout_, "  name \"asm.js compilation\"\n");
        fprintf(spewout_, "  method \"asm.js compilation\"\n");
    }
    fprintf(spewout_, "  date %d\n", (int)time(nullptr));
    fprintf(spewout_, "end_compilation\n");
}

void
C1Spewer::spewPass(const char* pass)
{
    if (!spewout_)
        return;

    fprintf(spewout_, "begin_cfg\n");
    fprintf(spewout_, "  name \"%s\"\n", pass);

    for (MBasicBlockIterator block(graph->begin()); block != graph->end(); block++)
        spewPass(spewout_, *block);

    fprintf(spewout_, "end_cfg\n");
    fflush(spewout_);
}

void
C1Spewer::spewIntervals(const char* pass, LinearScanAllocator* regalloc)
{
    if (!spewout_)
        return;

    fprintf(spewout_, "begin_intervals\n");
    fprintf(spewout_, " name \"%s\"\n", pass);

    size_t nextId = 0x4000;
    for (MBasicBlockIterator block(graph->begin()); block != graph->end(); block++)
        spewIntervals(spewout_, *block, regalloc, nextId);

    fprintf(spewout_, "end_intervals\n");
    fflush(spewout_);
}

void
C1Spewer::endFunction()
{
}

void
C1Spewer::finish()
{
    if (spewout_)
        fclose(spewout_);
}

static void
DumpDefinition(FILE* fp, MDefinition* def)
{
    fprintf(fp, "      ");
    fprintf(fp, "%u %u ", def->id(), unsigned(def->useCount()));
    def->printName(fp);
    fprintf(fp, " ");
    def->printOpcode(fp);
    fprintf(fp, " <|@\n");
}

static void
DumpLIR(FILE* fp, LNode* ins)
{
    fprintf(fp, "      ");
    fprintf(fp, "%d ", ins->id());
    ins->dump(fp);
    fprintf(fp, " <|@\n");
}

void
C1Spewer::spewIntervals(FILE* fp, LinearScanAllocator* regalloc, LNode* ins, size_t& nextId)
{
    for (size_t k = 0; k < ins->numDefs(); k++) {
        uint32_t id = ins->getDef(k)->virtualRegister();
        VirtualRegister* vreg = &regalloc->vregs[id];

        for (size_t i = 0; i < vreg->numIntervals(); i++) {
            LiveInterval* live = vreg->getInterval(i);
            if (live->numRanges()) {
                fprintf(fp, "%d object \"", (i == 0) ? id : int32_t(nextId++));
                fprintf(fp, "%s", live->getAllocation()->toString());
                fprintf(fp, "\" %d -1", id);
                for (size_t j = 0; j < live->numRanges(); j++) {
                    fprintf(fp, " [%u, %u[", live->getRange(j)->from.bits(),
                            live->getRange(j)->to.bits());
                }
                for (UsePositionIterator usePos(live->usesBegin()); usePos != live->usesEnd(); usePos++)
                    fprintf(fp, " %u M", usePos->pos.bits());
                fprintf(fp, " \"\"\n");
            }
        }
    }
}

void
C1Spewer::spewIntervals(FILE* fp, MBasicBlock* block, LinearScanAllocator* regalloc, size_t& nextId)
{
    LBlock* lir = block->lir();
    if (!lir)
        return;

    for (size_t i = 0; i < lir->numPhis(); i++)
        spewIntervals(fp, regalloc, lir->getPhi(i), nextId);

    for (LInstructionIterator ins = lir->begin(); ins != lir->end(); ins++)
        spewIntervals(fp, regalloc, *ins, nextId);
}

void
C1Spewer::spewPass(FILE* fp, MBasicBlock* block)
{
    fprintf(fp, "  begin_block\n");
    fprintf(fp, "    name \"B%d\"\n", block->id());
    fprintf(fp, "    from_bci -1\n");
    fprintf(fp, "    to_bci -1\n");

    fprintf(fp, "    predecessors");
    for (uint32_t i = 0; i < block->numPredecessors(); i++) {
        MBasicBlock* pred = block->getPredecessor(i);
        fprintf(fp, " \"B%d\"", pred->id());
    }
    fprintf(fp, "\n");

    fprintf(fp, "    successors");
    for (uint32_t i = 0; i < block->numSuccessors(); i++) {
        MBasicBlock* successor = block->getSuccessor(i);
        fprintf(fp, " \"B%d\"", successor->id());
    }
    fprintf(fp, "\n");

    fprintf(fp, "    xhandlers\n");
    fprintf(fp, "    flags\n");

    if (block->lir() && block->lir()->begin() != block->lir()->end()) {
        fprintf(fp, "    first_lir_id %d\n", block->lir()->firstId());
        fprintf(fp, "    last_lir_id %d\n", block->lir()->lastId());
    }

    fprintf(fp, "    begin_states\n");

    if (block->entryResumePoint()) {
        fprintf(fp, "      begin_locals\n");
        fprintf(fp, "        size %d\n", (int)block->numEntrySlots());
        fprintf(fp, "        method \"None\"\n");
        for (uint32_t i = 0; i < block->numEntrySlots(); i++) {
            MDefinition* ins = block->getEntrySlot(i);
            fprintf(fp, "        ");
            fprintf(fp, "%d ", i);
            if (ins->isUnused())
                fprintf(fp, "unused");
            else
                ins->printName(fp);
            fprintf(fp, "\n");
        }
        fprintf(fp, "      end_locals\n");
    }
    fprintf(fp, "    end_states\n");

    fprintf(fp, "    begin_HIR\n");
    for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++)
        DumpDefinition(fp, *phi);
    for (MInstructionIterator i(block->begin()); i != block->end(); i++)
        DumpDefinition(fp, *i);
    fprintf(fp, "    end_HIR\n");

    if (block->lir()) {
        fprintf(fp, "    begin_LIR\n");
        for (size_t i = 0; i < block->lir()->numPhis(); i++)
            DumpLIR(fp, block->lir()->getPhi(i));
        for (LInstructionIterator i(block->lir()->begin()); i != block->lir()->end(); i++)
            DumpLIR(fp, *i);
        fprintf(fp, "    end_LIR\n");
    }

    fprintf(fp, "  end_block\n");
}

#endif /* DEBUG */


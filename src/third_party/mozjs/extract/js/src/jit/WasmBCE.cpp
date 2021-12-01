/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "jit/WasmBCE.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"
#include "wasm/WasmTypes.h"

using namespace js;
using namespace js::jit;
using namespace mozilla;

typedef js::HashMap<uint32_t, MDefinition*, DefaultHasher<uint32_t>, SystemAllocPolicy>
    LastSeenMap;

// The Wasm Bounds Check Elimination (BCE) pass looks for bounds checks
// on SSA values that have already been checked. (in the same block or in a
// dominating block). These bounds checks are redundant and thus eliminated.
//
// Note: This is safe in the presense of dynamic memory sizes as long as they
// can ONLY GROW. If we allow SHRINKING the heap, this pass should be
// RECONSIDERED.
//
// TODO (dbounov): Are there a lot of cases where there is no single dominating
// check, but a set of checks that together dominate a redundant check?
//
// TODO (dbounov): Generalize to constant additions relative to one base
bool
jit::EliminateBoundsChecks(MIRGenerator* mir, MIRGraph& graph)
{
    // Map for dominating block where a given definition was checked
    LastSeenMap lastSeen;
    if (!lastSeen.init())
        return false;

    for (ReversePostorderIterator bIter(graph.rpoBegin()); bIter != graph.rpoEnd(); bIter++) {
        MBasicBlock* block = *bIter;
        for (MDefinitionIterator dIter(block); dIter;) {
            MDefinition* def = *dIter++;

            switch (def->op()) {
              case MDefinition::Opcode::WasmBoundsCheck: {
                MWasmBoundsCheck* bc = def->toWasmBoundsCheck();
                MDefinition* addr = bc->index();

                // Eliminate constant-address bounds checks to addresses below
                // the heap minimum.
                //
                // The payload of the MConstant will be Double if the constant
                // result is above 2^31-1, but we don't care about that for BCE.

#ifndef WASM_HUGE_MEMORY
                MOZ_ASSERT(wasm::MaxMemoryAccessSize < wasm::GuardSize,
                           "Guard page handles partial out-of-bounds");
#endif

                if (addr->isConstant() && addr->toConstant()->type() == MIRType::Int32 &&
                    uint32_t(addr->toConstant()->toInt32()) < mir->minWasmHeapLength())
                {
                    bc->setRedundant();
                    if (JitOptions.spectreIndexMasking)
                        bc->replaceAllUsesWith(addr);
                    else
                        MOZ_ASSERT(!bc->hasUses());
                }
                else
                {
                    LastSeenMap::AddPtr ptr = lastSeen.lookupForAdd(addr->id());
                    if (ptr) {
                        MDefinition* prevCheckOrPhi = ptr->value();
                        if (prevCheckOrPhi->block()->dominates(block)) {
                            bc->setRedundant();
                            if (JitOptions.spectreIndexMasking)
                                bc->replaceAllUsesWith(prevCheckOrPhi);
                            else
                                MOZ_ASSERT(!bc->hasUses());
                        }
                    } else {
                        if (!lastSeen.add(ptr, addr->id(), def))
                            return false;
                    }
                }
                break;
              }
              case MDefinition::Opcode::Phi: {
                MPhi* phi = def->toPhi();
                bool phiChecked = true;

                MOZ_ASSERT(phi->numOperands() > 0);

                // If all incoming values to a phi node are safe (i.e. have a
                // check that dominates this block) then we can consider this
                // phi node checked.
                //
                // Note that any phi that is part of a cycle
                // will not be "safe" since the value coming on the backedge
                // cannot be in lastSeen because its block hasn't been traversed yet.
                for (int i = 0, nOps = phi->numOperands(); i < nOps; i++) {
                    MDefinition* src = phi->getOperand(i);

                    if (JitOptions.spectreIndexMasking) {
                        if (src->isWasmBoundsCheck())
                            src = src->toWasmBoundsCheck()->index();
                    } else {
                        MOZ_ASSERT(!src->isWasmBoundsCheck());
                    }

                    LastSeenMap::Ptr checkPtr = lastSeen.lookup(src->id());
                    if (!checkPtr || !checkPtr->value()->block()->dominates(block)) {
                        phiChecked = false;
                        break;
                    }
                }

                if (phiChecked) {
                    if (!lastSeen.put(def->id(), def))
                        return false;
                }

                break;
              }
              default:
                break;
            }
        }
    }

    return true;
}

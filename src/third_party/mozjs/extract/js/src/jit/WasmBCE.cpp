/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/WasmBCE.h"

#include "jit/JitSpewer.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace js::jit;

using LastSeenMap = js::HashMap<uint32_t, MDefinition*, DefaultHasher<uint32_t>,
                                SystemAllocPolicy>;

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
bool jit::EliminateBoundsChecks(const MIRGenerator* mir, MIRGraph& graph) {
  JitSpew(JitSpew_WasmBCE, "Begin");
  // Map for dominating block where a given definition was checked
  LastSeenMap lastSeen;

  for (ReversePostorderIterator bIter(graph.rpoBegin());
       bIter != graph.rpoEnd(); bIter++) {
    MBasicBlock* block = *bIter;
    for (MDefinitionIterator dIter(block); dIter;) {
      MDefinition* def = *dIter++;

      switch (def->op()) {
        case MDefinition::Opcode::WasmBoundsCheck: {
          MWasmBoundsCheck* bc = def->toWasmBoundsCheck();
          MDefinition* addr = bc->index();

          // We only support bounds check elimination on wasm memory 0, not
          // other memories or tables. See bug 1625891.
          if (!bc->isMemory0()) {
            continue;
          }

          // Eliminate constant-address memory bounds checks to addresses below
          // the heap minimum.
          //
          // The payload of the MConstant will be Double if the constant
          // result is above 2^31-1, but we don't care about that for BCE.

          if (addr->isConstant() &&
              ((addr->toConstant()->type() == MIRType::Int32 &&
                uint64_t(addr->toConstant()->toInt32()) <
                    mir->minWasmMemory0Length()) ||
               (addr->toConstant()->type() == MIRType::Int64 &&
                uint64_t(addr->toConstant()->toInt64()) <
                    mir->minWasmMemory0Length()))) {
            bc->setRedundant();
            if (JitOptions.spectreIndexMasking) {
              bc->replaceAllUsesWith(addr);
            } else {
              MOZ_ASSERT(!bc->hasUses());
            }
          } else {
            LastSeenMap::AddPtr ptr = lastSeen.lookupForAdd(addr->id());
            if (ptr) {
              MDefinition* prevCheckOrPhi = ptr->value();
              if (prevCheckOrPhi->block()->dominates(block)) {
                bc->setRedundant();
                if (JitOptions.spectreIndexMasking) {
                  bc->replaceAllUsesWith(prevCheckOrPhi);
                } else {
                  MOZ_ASSERT(!bc->hasUses());
                }
              }
            } else {
              if (!lastSeen.add(ptr, addr->id(), def)) {
                return false;
              }
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
              if (src->isWasmBoundsCheck()) {
                src = src->toWasmBoundsCheck()->index();
              }
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
            if (!lastSeen.put(def->id(), def)) {
              return false;
            }
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

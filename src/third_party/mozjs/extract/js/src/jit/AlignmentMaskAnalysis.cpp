/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AlignmentMaskAnalysis.h"
#include "jit/MIR-wasm.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace jit;

static bool IsAlignmentMask(uint32_t m) {
  // Test whether m is just leading ones and trailing zeros.
  return (-m & ~m) == 0;
}

static void AnalyzeAsmHeapAddress(MDefinition* ptr, MIRGraph& graph) {
  // Fold (a+i)&m to (a&m)+i, provided that this doesn't change the result,
  // since the users of the BitAnd include heap accesses. This will expose
  // the redundancy for GVN when expressions like this:
  //   a&m
  //   (a+1)&m,
  //   (a+2)&m,
  // are transformed into this:
  //   a&m
  //   (a&m)+1
  //   (a&m)+2
  // and it will allow the constants to be folded by the
  // EffectiveAddressAnalysis pass.
  //
  // Putting the add on the outside might seem like it exposes other users of
  // the expression to the possibility of i32 overflow, if we aren't in wasm
  // and they aren't naturally truncating. However, since we use MAdd::New
  // with MIRType::Int32, we make sure that the value is truncated, just as it
  // would be by the MBitAnd.

  MOZ_ASSERT(IsCompilingWasm());

  if (!ptr->isBitAnd()) {
    return;
  }

  MDefinition* lhs = ptr->toBitAnd()->getOperand(0);
  MDefinition* rhs = ptr->toBitAnd()->getOperand(1);
  if (lhs->isConstant()) {
    std::swap(lhs, rhs);
  }
  if (!lhs->isAdd() || !rhs->isConstant()) {
    return;
  }

  MDefinition* op0 = lhs->toAdd()->getOperand(0);
  MDefinition* op1 = lhs->toAdd()->getOperand(1);
  if (op0->isConstant()) {
    std::swap(op0, op1);
  }
  if (!op1->isConstant()) {
    return;
  }

  uint32_t i = op1->toConstant()->toInt32();
  uint32_t m = rhs->toConstant()->toInt32();
  if (!IsAlignmentMask(m) || (i & m) != i) {
    return;
  }

  // The pattern was matched! Produce the replacement expression.
  MInstruction* and_ = MBitAnd::New(graph.alloc(), op0, rhs, MIRType::Int32);
  ptr->block()->insertBefore(ptr->toBitAnd(), and_);
  auto* add = MAdd::New(graph.alloc(), and_, op1, TruncateKind::Truncate);
  ptr->block()->insertBefore(ptr->toBitAnd(), add);
  ptr->replaceAllUsesWith(add);
  ptr->block()->discard(ptr->toBitAnd());
}

bool AlignmentMaskAnalysis::analyze() {
  for (ReversePostorderIterator block(graph_.rpoBegin());
       block != graph_.rpoEnd(); block++) {
    for (MInstructionIterator i = block->begin(); i != block->end(); i++) {
      if (!graph_.alloc().ensureBallast()) {
        return false;
      }

      // Note that we don't check for MWasmCompareExchangeHeap
      // or MWasmAtomicBinopHeap, because the backend and the OOB
      // mechanism don't support non-zero offsets for them yet.
      if (i->isAsmJSLoadHeap()) {
        AnalyzeAsmHeapAddress(i->toAsmJSLoadHeap()->base(), graph_);
      } else if (i->isAsmJSStoreHeap()) {
        AnalyzeAsmHeapAddress(i->toAsmJSStoreHeap()->base(), graph_);
      }
    }
  }
  return true;
}

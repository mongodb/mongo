/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/EdgeCaseAnalysis.h"

#include "jit/MIR-wasm.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace js::jit;

EdgeCaseAnalysis::EdgeCaseAnalysis(const MIRGenerator* mir, MIRGraph& graph)
    : mir(mir), graph(graph) {}

bool EdgeCaseAnalysis::analyzeLate() {
  // Renumber definitions for NeedNegativeZeroCheck under
  // analyzeEdgeCasesBackward.
  uint32_t nextId = 0;

  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); block++) {
    for (MDefinitionIterator iter(*block); iter; iter++) {
      if (mir->shouldCancel("Analyze Late (first loop)")) {
        return false;
      }

      iter->setId(nextId++);
      iter->analyzeEdgeCasesForward();
    }
    block->lastIns()->setId(nextId++);
  }

  for (PostorderIterator block(graph.poBegin()); block != graph.poEnd();
       block++) {
    for (MInstructionReverseIterator riter(block->rbegin());
         riter != block->rend(); riter++) {
      if (mir->shouldCancel("Analyze Late (second loop)")) {
        return false;
      }

      riter->analyzeEdgeCasesBackward();
    }
  }

  return true;
}

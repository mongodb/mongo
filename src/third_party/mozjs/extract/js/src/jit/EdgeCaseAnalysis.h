/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_EdgeCaseAnalysis_h
#define jit_EdgeCaseAnalysis_h

namespace js {
namespace jit {

class MIRGenerator;
class MIRGraph;

class EdgeCaseAnalysis {
  const MIRGenerator* mir;
  MIRGraph& graph;

 public:
  EdgeCaseAnalysis(const MIRGenerator* mir, MIRGraph& graph);
  [[nodiscard]] bool analyzeLate();
};

}  // namespace jit
}  // namespace js

#endif /* jit_EdgeCaseAnalysis_h */

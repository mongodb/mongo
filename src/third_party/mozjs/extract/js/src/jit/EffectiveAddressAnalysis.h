/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_EffectiveAddressAnalysis_h
#define jit_EffectiveAddressAnalysis_h

namespace js {
namespace jit {

class MIRGenerator;
class MIRGraph;

class EffectiveAddressAnalysis {
  const MIRGenerator* mir_;
  MIRGraph& graph_;

  template <typename AsmJSMemoryAccess>
  void analyzeAsmJSHeapAccess(AsmJSMemoryAccess* ins);

 public:
  EffectiveAddressAnalysis(const MIRGenerator* mir, MIRGraph& graph)
      : mir_(mir), graph_(graph) {}

  [[nodiscard]] bool analyze();
};

} /* namespace jit */
} /* namespace js */

#endif /* jit_EffectiveAddressAnalysis_h */

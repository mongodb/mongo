/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AliasAnalysis_h
#define jit_AliasAnalysis_h

#include "jit/MIR-wasm.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"

namespace js {
namespace jit {

class LoopAliasInfo;

class AliasAnalysis {
  const MIRGenerator* mir;
  MIRGraph& graph_;
  LoopAliasInfo* loop_;

  void spewDependencyList();

  TempAllocator& alloc() const { return graph_.alloc(); }

 public:
  AliasAnalysis(const MIRGenerator* mir, MIRGraph& graph)
      : mir(mir), graph_(graph), loop_(nullptr) {}

  [[nodiscard]] bool analyze();
};

// Iterates over the flags in an AliasSet.
class AliasSetIterator {
 private:
  uint32_t flags;
  unsigned pos;

 public:
  explicit AliasSetIterator(AliasSet set) : flags(set.flags()), pos(0) {
    while (flags && (flags & 1) == 0) {
      flags >>= 1;
      pos++;
    }
  }
  AliasSetIterator& operator++(int) {
    do {
      flags >>= 1;
      pos++;
    } while (flags && (flags & 1) == 0);
    return *this;
  }
  explicit operator bool() const { return !!flags; }
  unsigned operator*() const {
    MOZ_ASSERT(pos < AliasSet::NumCategories);
    return pos;
  }
};

}  // namespace jit
}  // namespace js

#endif /* jit_AliasAnalysis_h */

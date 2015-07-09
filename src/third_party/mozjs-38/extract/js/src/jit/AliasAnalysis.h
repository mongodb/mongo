/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AliasAnalysis_h
#define jit_AliasAnalysis_h

#include "jit/MIR.h"
#include "jit/MIRGraph.h"

namespace js {
namespace jit {

class LoopAliasInfo;
class MIRGraph;

class AliasAnalysis
{
    MIRGenerator* mir;
    MIRGraph& graph_;
    LoopAliasInfo* loop_;

    TempAllocator& alloc() const {
        return graph_.alloc();
    }

  public:
    AliasAnalysis(MIRGenerator* mir, MIRGraph& graph);
    bool analyze();
};

} // namespace jit
} // namespace js

#endif /* jit_AliasAnalysis_h */

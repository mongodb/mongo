/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_EdgeCaseAnalysis_h
#define jit_EdgeCaseAnalysis_h

#include "jit/MIRGenerator.h"

namespace js {
namespace jit {

class MIRGraph;

class EdgeCaseAnalysis
{
    MIRGenerator* mir;
    MIRGraph& graph;

  public:
    EdgeCaseAnalysis(MIRGenerator* mir, MIRGraph& graph);
    bool analyzeLate();
};


} // namespace jit
} // namespace js

#endif /* jit_EdgeCaseAnalysis_h */

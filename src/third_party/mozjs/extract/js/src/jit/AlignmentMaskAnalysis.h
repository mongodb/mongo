/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AlignmentMaskAnalysis_h
#define jit_AlignmentMaskAnalysis_h

#include "mozilla/Attributes.h"

namespace js {
namespace jit {

class MIRGraph;

class AlignmentMaskAnalysis
{
    MIRGraph& graph_;

  public:
    explicit AlignmentMaskAnalysis(MIRGraph& graph)
      : graph_(graph)
    {}

    MOZ_MUST_USE bool analyze();
};

} /* namespace jit */
} /* namespace js */

#endif /* jit_AlignmentMaskAnalysis_h */

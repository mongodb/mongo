/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JSONSpewer_h
#define jit_JSONSpewer_h

#ifdef JS_JITSPEW

#include <stdio.h>

#include "js/TypeDecls.h"
#include "vm/JSONPrinter.h"

namespace js {
namespace jit {

class BacktrackingAllocator;
class MDefinition;
class MIRGraph;
class MResumePoint;
class LNode;

class JSONSpewer : JSONPrinter
{
  public:
    explicit JSONSpewer(GenericPrinter& out)
      : JSONPrinter(out)
    { }

    void beginFunction(JSScript* script);
    void beginPass(const char * pass);
    void spewMDef(MDefinition* def);
    void spewMResumePoint(MResumePoint* rp);
    void spewMIR(MIRGraph* mir);
    void spewLIns(LNode* ins);
    void spewLIR(MIRGraph* mir);
    void spewRanges(BacktrackingAllocator* regalloc);
    void endPass();
    void endFunction();
};

} // namespace jit
} // namespace js

#endif /* JS_JITSPEW */

#endif /* jit_JSONSpewer_h */

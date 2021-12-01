/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_C1Spewer_h
#define jit_C1Spewer_h

#ifdef JS_JITSPEW

#include "NamespaceImports.h"

#include "js/RootingAPI.h"
#include "vm/Printer.h"

namespace js {
namespace jit {

class BacktrackingAllocator;
class MBasicBlock;
class MIRGraph;
class LNode;

class C1Spewer
{
    MIRGraph* graph;
    GenericPrinter& out_;

  public:
    explicit C1Spewer(GenericPrinter& out)
      : graph(nullptr), out_(out)
    { }

    void beginFunction(MIRGraph* graph, JSScript* script);
    void spewPass(const char* pass);
    void spewRanges(const char* pass, BacktrackingAllocator* regalloc);
    void endFunction();

  private:
    void spewPass(GenericPrinter& out, MBasicBlock* block);
    void spewRanges(GenericPrinter& out, BacktrackingAllocator* regalloc, LNode* ins);
    void spewRanges(GenericPrinter& out, MBasicBlock* block, BacktrackingAllocator* regalloc);
};

} // namespace jit
} // namespace js

#endif /* JS_JITSPEW */

#endif /* jit_C1Spewer_h */

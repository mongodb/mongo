/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonAnalysis_h
#define jit_IonAnalysis_h

// This file declares various analysis passes that operate on MIR.

#include "jit/JitAllocPolicy.h"
#include "jit/MIR.h"

namespace js {
namespace jit {

class MIRGenerator;
class MIRGraph;

void
FoldTests(MIRGraph& graph);

bool
SplitCriticalEdges(MIRGraph& graph);

enum Observability {
    ConservativeObservability,
    AggressiveObservability
};

bool
EliminatePhis(MIRGenerator* mir, MIRGraph& graph, Observability observe);

size_t
MarkLoopBlocks(MIRGraph& graph, MBasicBlock* header, bool* canOsr);

void
UnmarkLoopBlocks(MIRGraph& graph, MBasicBlock* header);

bool
MakeLoopsContiguous(MIRGraph& graph);

bool
EliminateDeadResumePointOperands(MIRGenerator* mir, MIRGraph& graph);

bool
EliminateDeadCode(MIRGenerator* mir, MIRGraph& graph);

bool
ApplyTypeInformation(MIRGenerator* mir, MIRGraph& graph);

bool
MakeMRegExpHoistable(MIRGraph& graph);

bool
RenumberBlocks(MIRGraph& graph);

bool
AccountForCFGChanges(MIRGenerator* mir, MIRGraph& graph, bool updateAliasAnalysis);

bool
RemoveUnmarkedBlocks(MIRGenerator* mir, MIRGraph& graph, uint32_t numMarkedBlocks);

void
ClearDominatorTree(MIRGraph& graph);

bool
BuildDominatorTree(MIRGraph& graph);

bool
BuildPhiReverseMapping(MIRGraph& graph);

void
AssertBasicGraphCoherency(MIRGraph& graph);

void
AssertGraphCoherency(MIRGraph& graph);

void
AssertExtendedGraphCoherency(MIRGraph& graph);

bool
EliminateRedundantChecks(MIRGraph& graph);

void
AddKeepAliveInstructions(MIRGraph& graph);

class MDefinition;

// Simple linear sum of the form 'n' or 'x + n'.
struct SimpleLinearSum
{
    MDefinition* term;
    int32_t constant;

    SimpleLinearSum(MDefinition* term, int32_t constant)
        : term(term), constant(constant)
    {}
};

SimpleLinearSum
ExtractLinearSum(MDefinition* ins);

bool
ExtractLinearInequality(MTest* test, BranchDirection direction,
                        SimpleLinearSum* plhs, MDefinition** prhs, bool* plessEqual);

struct LinearTerm
{
    MDefinition* term;
    int32_t scale;

    LinearTerm(MDefinition* term, int32_t scale)
      : term(term), scale(scale)
    {
    }
};

// General linear sum of the form 'x1*n1 + x2*n2 + ... + n'
class LinearSum
{
  public:
    explicit LinearSum(TempAllocator& alloc)
      : terms_(alloc),
        constant_(0)
    {
    }

    LinearSum(const LinearSum& other)
      : terms_(other.terms_.allocPolicy()),
        constant_(other.constant_)
    {
        terms_.appendAll(other.terms_);
    }

    // These return false on an integer overflow, and afterwards the sum must
    // not be used.
    bool multiply(int32_t scale);
    bool add(const LinearSum& other, int32_t scale = 1);
    bool add(SimpleLinearSum other, int32_t scale = 1);
    bool add(MDefinition* term, int32_t scale);
    bool add(int32_t constant);

    // Unlike the above function, on failure this leaves the sum unchanged and
    // it can still be used.
    bool divide(int32_t scale);

    int32_t constant() const { return constant_; }
    size_t numTerms() const { return terms_.length(); }
    LinearTerm term(size_t i) const { return terms_[i]; }
    void replaceTerm(size_t i, MDefinition* def) { terms_[i].term = def; }

    void print(Sprinter& sp) const;
    void dump(FILE*) const;
    void dump() const;

  private:
    Vector<LinearTerm, 2, JitAllocPolicy> terms_;
    int32_t constant_;
};

// Convert all components of a linear sum (except, optionally, the constant)
// and add any new instructions to the end of block.
MDefinition*
ConvertLinearSum(TempAllocator& alloc, MBasicBlock* block, const LinearSum& sum,
                 bool convertConstant = false);

// Convert the test 'sum >= 0' to a comparison, adding any necessary
// instructions to the end of block.
MCompare*
ConvertLinearInequality(TempAllocator& alloc, MBasicBlock* block, const LinearSum& sum);

bool
AnalyzeNewScriptDefiniteProperties(JSContext* cx, JSFunction* fun,
                                   ObjectGroup* group, HandlePlainObject baseobj,
                                   Vector<TypeNewScript::Initializer>* initializerList);

bool
AnalyzeArgumentsUsage(JSContext* cx, JSScript* script);

bool
DeadIfUnused(const MDefinition* def);

bool
IsDiscardable(const MDefinition* def);

} // namespace jit
} // namespace js

#endif /* jit_IonAnalysis_h */

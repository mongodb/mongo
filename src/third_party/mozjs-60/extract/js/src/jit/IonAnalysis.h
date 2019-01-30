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

MOZ_MUST_USE bool
PruneUnusedBranches(MIRGenerator* mir, MIRGraph& graph);

MOZ_MUST_USE bool
FoldTests(MIRGraph& graph);

MOZ_MUST_USE bool
FoldEmptyBlocks(MIRGraph& graph);

MOZ_MUST_USE bool
SplitCriticalEdges(MIRGraph& graph);

bool
IsUint32Type(const MDefinition* def);

enum Observability {
    ConservativeObservability,
    AggressiveObservability
};

MOZ_MUST_USE bool
EliminatePhis(MIRGenerator* mir, MIRGraph& graph, Observability observe);

size_t
MarkLoopBlocks(MIRGraph& graph, MBasicBlock* header, bool* canOsr);

void
UnmarkLoopBlocks(MIRGraph& graph, MBasicBlock* header);

MOZ_MUST_USE bool
MakeLoopsContiguous(MIRGraph& graph);

MOZ_MUST_USE bool
EliminateDeadResumePointOperands(MIRGenerator* mir, MIRGraph& graph);

MOZ_MUST_USE bool
EliminateDeadCode(MIRGenerator* mir, MIRGraph& graph);

MOZ_MUST_USE bool
ApplyTypeInformation(MIRGenerator* mir, MIRGraph& graph);

MOZ_MUST_USE bool
MakeMRegExpHoistable(MIRGenerator* mir, MIRGraph& graph);

void
RenumberBlocks(MIRGraph& graph);

MOZ_MUST_USE bool
AccountForCFGChanges(MIRGenerator* mir, MIRGraph& graph, bool updateAliasAnalysis,
                     bool underValueNumberer = false);

MOZ_MUST_USE bool
RemoveUnmarkedBlocks(MIRGenerator* mir, MIRGraph& graph, uint32_t numMarkedBlocks);

MOZ_MUST_USE bool
CreateMIRRootList(IonBuilder& builder);

void
ClearDominatorTree(MIRGraph& graph);

MOZ_MUST_USE bool
BuildDominatorTree(MIRGraph& graph);

MOZ_MUST_USE bool
BuildPhiReverseMapping(MIRGraph& graph);

void
AssertBasicGraphCoherency(MIRGraph& graph, bool force = false);

void
AssertGraphCoherency(MIRGraph& graph, bool force = false);

void
AssertExtendedGraphCoherency(MIRGraph& graph, bool underValueNumberer = false, bool force = false);

MOZ_MUST_USE bool
EliminateRedundantChecks(MIRGraph& graph);

MOZ_MUST_USE bool
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

// Math done in a Linear sum can either be in a modulo space, in which case
// overflow are wrapped around, or they can be computed in the integer-space in
// which case we have to check that no overflow can happen when summing
// constants.
//
// When the caller ignores which space it is, the definition would be used to
// deduce it.
enum class MathSpace {
    Modulo,
    Infinite,
    Unknown
};

SimpleLinearSum
ExtractLinearSum(MDefinition* ins, MathSpace space = MathSpace::Unknown);

MOZ_MUST_USE bool
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
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!terms_.appendAll(other.terms_))
            oomUnsafe.crash("LinearSum::LinearSum");
    }

    // These return false on an integer overflow, and afterwards the sum must
    // not be used.
    MOZ_MUST_USE bool multiply(int32_t scale);
    MOZ_MUST_USE bool add(const LinearSum& other, int32_t scale = 1);
    MOZ_MUST_USE bool add(SimpleLinearSum other, int32_t scale = 1);
    MOZ_MUST_USE bool add(MDefinition* term, int32_t scale);
    MOZ_MUST_USE bool add(int32_t constant);

    // Unlike the above function, on failure this leaves the sum unchanged and
    // it can still be used.
    MOZ_MUST_USE bool divide(uint32_t scale);

    int32_t constant() const { return constant_; }
    size_t numTerms() const { return terms_.length(); }
    LinearTerm term(size_t i) const { return terms_[i]; }
    void replaceTerm(size_t i, MDefinition* def) { terms_[i].term = def; }

    void dump(GenericPrinter& out) const;
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

MOZ_MUST_USE bool
AnalyzeNewScriptDefiniteProperties(JSContext* cx, HandleFunction fun,
                                   ObjectGroup* group, HandlePlainObject baseobj,
                                   Vector<TypeNewScript::Initializer>* initializerList);

MOZ_MUST_USE bool
AnalyzeArgumentsUsage(JSContext* cx, JSScript* script);

bool
DeadIfUnused(const MDefinition* def);

bool
IsDiscardable(const MDefinition* def);

void
DumpMIRExpressions(MIRGraph& graph);

} // namespace jit
} // namespace js

#endif /* jit_IonAnalysis_h */

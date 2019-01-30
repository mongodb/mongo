/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/RangeAnalysis.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/Ion.h"
#include "jit/IonAnalysis.h"
#include "jit/JitSpewer.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"
#include "js/Conversions.h"
#include "vm/ArgumentsObject.h"
#include "vm/TypedArrayObject.h"

#include "vm/BytecodeUtil-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Abs;
using mozilla::CountLeadingZeroes32;
using mozilla::NumberEqualsInt32;
using mozilla::ExponentComponent;
using mozilla::FloorLog2;
using mozilla::IsInfinite;
using mozilla::IsNaN;
using mozilla::IsNegativeZero;
using mozilla::NegativeInfinity;
using mozilla::PositiveInfinity;
using mozilla::Swap;
using JS::GenericNaN;
using JS::ToInt32;

// This algorithm is based on the paper "Eliminating Range Checks Using
// Static Single Assignment Form" by Gough and Klaren.
//
// We associate a range object with each SSA name, and the ranges are consulted
// in order to determine whether overflow is possible for arithmetic
// computations.
//
// An important source of range information that requires care to take
// advantage of is conditional control flow. Consider the code below:
//
// if (x < 0) {
//   y = x + 2000000000;
// } else {
//   if (x < 1000000000) {
//     y = x * 2;
//   } else {
//     y = x - 3000000000;
//   }
// }
//
// The arithmetic operations in this code cannot overflow, but it is not
// sufficient to simply associate each name with a range, since the information
// differs between basic blocks. The traditional dataflow approach would be
// associate ranges with (name, basic block) pairs. This solution is not
// satisfying, since we lose the benefit of SSA form: in SSA form, each
// definition has a unique name, so there is no need to track information about
// the control flow of the program.
//
// The approach used here is to add a new form of pseudo operation called a
// beta node, which associates range information with a value. These beta
// instructions take one argument and additionally have an auxiliary constant
// range associated with them. Operationally, beta nodes are just copies, but
// the invariant expressed by beta node copies is that the output will fall
// inside the range given by the beta node.  Gough and Klaeren refer to SSA
// extended with these beta nodes as XSA form. The following shows the example
// code transformed into XSA form:
//
// if (x < 0) {
//   x1 = Beta(x, [INT_MIN, -1]);
//   y1 = x1 + 2000000000;
// } else {
//   x2 = Beta(x, [0, INT_MAX]);
//   if (x2 < 1000000000) {
//     x3 = Beta(x2, [INT_MIN, 999999999]);
//     y2 = x3*2;
//   } else {
//     x4 = Beta(x2, [1000000000, INT_MAX]);
//     y3 = x4 - 3000000000;
//   }
//   y4 = Phi(y2, y3);
// }
// y = Phi(y1, y4);
//
// We insert beta nodes for the purposes of range analysis (they might also be
// usefully used for other forms of bounds check elimination) and remove them
// after range analysis is performed. The remaining compiler phases do not ever
// encounter beta nodes.

static bool
IsDominatedUse(MBasicBlock* block, MUse* use)
{
    MNode* n = use->consumer();
    bool isPhi = n->isDefinition() && n->toDefinition()->isPhi();

    if (isPhi) {
        MPhi* phi = n->toDefinition()->toPhi();
        return block->dominates(phi->block()->getPredecessor(phi->indexOf(use)));
    }

    return block->dominates(n->block());
}

static inline void
SpewRange(MDefinition* def)
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Range) && def->type() != MIRType::None && def->range()) {
        JitSpewHeader(JitSpew_Range);
        Fprinter& out = JitSpewPrinter();
        def->printName(out);
        out.printf(" has range ");
        def->range()->dump(out);
    }
#endif
}

static inline void
SpewTruncate(MDefinition* def, MDefinition::TruncateKind kind, bool shouldClone)
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Range)) {
        JitSpewHeader(JitSpew_Range);
        Fprinter& out = JitSpewPrinter();
        out.printf("truncating ");
        def->printName(out);
        out.printf(" (kind: %s, clone: %d)\n", MDefinition::TruncateKindString(kind), shouldClone);
    }
#endif
}

TempAllocator&
RangeAnalysis::alloc() const
{
    return graph_.alloc();
}

void
RangeAnalysis::replaceDominatedUsesWith(MDefinition* orig, MDefinition* dom,
                                            MBasicBlock* block)
{
    for (MUseIterator i(orig->usesBegin()); i != orig->usesEnd(); ) {
        MUse* use = *i++;
        if (use->consumer() != dom && IsDominatedUse(block, use))
            use->replaceProducer(dom);
    }
}

bool
RangeAnalysis::addBetaNodes()
{
    JitSpew(JitSpew_Range, "Adding beta nodes");

    for (PostorderIterator i(graph_.poBegin()); i != graph_.poEnd(); i++) {
        MBasicBlock* block = *i;
        JitSpew(JitSpew_Range, "Looking at block %d", block->id());

        BranchDirection branch_dir;
        MTest* test = block->immediateDominatorBranch(&branch_dir);

        if (!test || !test->getOperand(0)->isCompare())
            continue;

        MCompare* compare = test->getOperand(0)->toCompare();

        if (!compare->isNumericComparison())
            continue;

        // TODO: support unsigned comparisons
        if (compare->compareType() == MCompare::Compare_UInt32)
            continue;

        MDefinition* left = compare->getOperand(0);
        MDefinition* right = compare->getOperand(1);
        double bound;
        double conservativeLower = NegativeInfinity<double>();
        double conservativeUpper = PositiveInfinity<double>();
        MDefinition* val = nullptr;

        JSOp jsop = compare->jsop();

        if (branch_dir == FALSE_BRANCH) {
            jsop = NegateCompareOp(jsop);
            conservativeLower = GenericNaN();
            conservativeUpper = GenericNaN();
        }

        MConstant* leftConst = left->maybeConstantValue();
        MConstant* rightConst = right->maybeConstantValue();
        if (leftConst && leftConst->isTypeRepresentableAsDouble()) {
            bound = leftConst->numberToDouble();
            val = right;
            jsop = ReverseCompareOp(jsop);
        } else if (rightConst && rightConst->isTypeRepresentableAsDouble()) {
            bound = rightConst->numberToDouble();
            val = left;
        } else if (left->type() == MIRType::Int32 && right->type() == MIRType::Int32) {
            MDefinition* smaller = nullptr;
            MDefinition* greater = nullptr;
            if (jsop == JSOP_LT) {
                smaller = left;
                greater = right;
            } else if (jsop == JSOP_GT) {
                smaller = right;
                greater = left;
            }
            if (smaller && greater) {
                if (!alloc().ensureBallast())
                    return false;

                MBeta* beta;
                beta = MBeta::New(alloc(), smaller,
                                  Range::NewInt32Range(alloc(), JSVAL_INT_MIN, JSVAL_INT_MAX-1));
                block->insertBefore(*block->begin(), beta);
                replaceDominatedUsesWith(smaller, beta, block);
                JitSpew(JitSpew_Range, "Adding beta node for smaller %d", smaller->id());
                beta = MBeta::New(alloc(), greater,
                                  Range::NewInt32Range(alloc(), JSVAL_INT_MIN+1, JSVAL_INT_MAX));
                block->insertBefore(*block->begin(), beta);
                replaceDominatedUsesWith(greater, beta, block);
                JitSpew(JitSpew_Range, "Adding beta node for greater %d", greater->id());
            }
            continue;
        } else {
            continue;
        }

        // At this point, one of the operands if the compare is a constant, and
        // val is the other operand.
        MOZ_ASSERT(val);

        Range comp;
        switch (jsop) {
          case JSOP_LE:
            comp.setDouble(conservativeLower, bound);
            break;
          case JSOP_LT:
            // For integers, if x < c, the upper bound of x is c-1.
            if (val->type() == MIRType::Int32) {
                int32_t intbound;
                if (NumberEqualsInt32(bound, &intbound) && SafeSub(intbound, 1, &intbound))
                    bound = intbound;
            }
            comp.setDouble(conservativeLower, bound);

            // Negative zero is not less than zero.
            if (bound == 0)
                comp.refineToExcludeNegativeZero();
            break;
          case JSOP_GE:
            comp.setDouble(bound, conservativeUpper);
            break;
          case JSOP_GT:
            // For integers, if x > c, the lower bound of x is c+1.
            if (val->type() == MIRType::Int32) {
                int32_t intbound;
                if (NumberEqualsInt32(bound, &intbound) && SafeAdd(intbound, 1, &intbound))
                    bound = intbound;
            }
            comp.setDouble(bound, conservativeUpper);

            // Negative zero is not greater than zero.
            if (bound == 0)
                comp.refineToExcludeNegativeZero();
            break;
          case JSOP_STRICTEQ:
            // A strict comparison can test for things other than numeric value.
            if (!compare->isNumericComparison())
                continue;
            // Otherwise fall through to handle JSOP_STRICTEQ the same as JSOP_EQ.
            MOZ_FALLTHROUGH;
          case JSOP_EQ:
            comp.setDouble(bound, bound);
            break;
          case JSOP_STRICTNE:
            // A strict comparison can test for things other than numeric value.
            if (!compare->isNumericComparison())
                continue;
            // Otherwise fall through to handle JSOP_STRICTNE the same as JSOP_NE.
            MOZ_FALLTHROUGH;
          case JSOP_NE:
            // Negative zero is not not-equal to zero.
            if (bound == 0) {
                comp.refineToExcludeNegativeZero();
                break;
            }
            continue; // well, we could have
                      // [-\inf, bound-1] U [bound+1, \inf] but we only use contiguous ranges.
          default:
            continue;
        }

        if (JitSpewEnabled(JitSpew_Range)) {
            JitSpewHeader(JitSpew_Range);
            Fprinter& out = JitSpewPrinter();
            out.printf("Adding beta node for %d with range ", val->id());
            comp.dump(out);
        }

        if (!alloc().ensureBallast())
            return false;

        MBeta* beta = MBeta::New(alloc(), val, new(alloc()) Range(comp));
        block->insertBefore(*block->begin(), beta);
        replaceDominatedUsesWith(val, beta, block);
    }

    return true;
}

bool
RangeAnalysis::removeBetaNodes()
{
    JitSpew(JitSpew_Range, "Removing beta nodes");

    for (PostorderIterator i(graph_.poBegin()); i != graph_.poEnd(); i++) {
        MBasicBlock* block = *i;
        for (MDefinitionIterator iter(*i); iter; ) {
            MDefinition* def = *iter++;
            if (def->isBeta()) {
                MDefinition* op = def->getOperand(0);
                JitSpew(JitSpew_Range, "Removing beta node %d for %d",
                        def->id(), op->id());
                def->justReplaceAllUsesWith(op);
                block->discardDef(def);
            } else {
                // We only place Beta nodes at the beginning of basic
                // blocks, so if we see something else, we can move on
                // to the next block.
                break;
            }
        }
    }
    return true;
}

void
SymbolicBound::dump(GenericPrinter& out) const
{
    if (loop)
        out.printf("[loop] ");
    sum.dump(out);
}

void
SymbolicBound::dump() const
{
    Fprinter out(stderr);
    dump(out);
    out.printf("\n");
    out.finish();
}

// Test whether the given range's exponent tells us anything that its lower
// and upper bound values don't.
static bool
IsExponentInteresting(const Range* r)
{
   // If it lacks either a lower or upper bound, the exponent is interesting.
   if (!r->hasInt32Bounds())
       return true;

   // Otherwise if there's no fractional part, the lower and upper bounds,
   // which are integers, are perfectly precise.
   if (!r->canHaveFractionalPart())
       return false;

   // Otherwise, if the bounds are conservatively rounded across a power-of-two
   // boundary, the exponent may imply a tighter range.
   return FloorLog2(Max(Abs(r->lower()), Abs(r->upper()))) > r->exponent();
}

void
Range::dump(GenericPrinter& out) const
{
    assertInvariants();

    // Floating-point or Integer subset.
    if (canHaveFractionalPart_)
        out.printf("F");
    else
        out.printf("I");

    out.printf("[");

    if (!hasInt32LowerBound_)
        out.printf("?");
    else
        out.printf("%d", lower_);
    if (symbolicLower_) {
        out.printf(" {");
        symbolicLower_->dump(out);
        out.printf("}");
    }

    out.printf(", ");

    if (!hasInt32UpperBound_)
        out.printf("?");
    else
        out.printf("%d", upper_);
    if (symbolicUpper_) {
        out.printf(" {");
        symbolicUpper_->dump(out);
        out.printf("}");
    }

    out.printf("]");

    bool includesNaN = max_exponent_ == IncludesInfinityAndNaN;
    bool includesNegativeInfinity = max_exponent_ >= IncludesInfinity && !hasInt32LowerBound_;
    bool includesPositiveInfinity = max_exponent_ >= IncludesInfinity && !hasInt32UpperBound_;
    bool includesNegativeZero = canBeNegativeZero_;

    if (includesNaN ||
        includesNegativeInfinity ||
        includesPositiveInfinity ||
        includesNegativeZero)
    {
        out.printf(" (");
        bool first = true;
        if (includesNaN) {
            if (first)
                first = false;
            else
                out.printf(" ");
            out.printf("U NaN");
        }
        if (includesNegativeInfinity) {
            if (first)
                first = false;
            else
                out.printf(" ");
            out.printf("U -Infinity");
        }
        if (includesPositiveInfinity) {
            if (first)
                first = false;
            else
                out.printf(" ");
            out.printf("U Infinity");
        }
        if (includesNegativeZero) {
            if (first)
                first = false;
            else
                out.printf(" ");
            out.printf("U -0");
        }
        out.printf(")");
    }
    if (max_exponent_ < IncludesInfinity && IsExponentInteresting(this))
        out.printf(" (< pow(2, %d+1))", max_exponent_);
}

void
Range::dump() const
{
    Fprinter out(stderr);
    dump(out);
    out.printf("\n");
    out.finish();
}

Range*
Range::intersect(TempAllocator& alloc, const Range* lhs, const Range* rhs, bool* emptyRange)
{
    *emptyRange = false;

    if (!lhs && !rhs)
        return nullptr;

    if (!lhs)
        return new(alloc) Range(*rhs);
    if (!rhs)
        return new(alloc) Range(*lhs);

    int32_t newLower = Max(lhs->lower_, rhs->lower_);
    int32_t newUpper = Min(lhs->upper_, rhs->upper_);

    // If upper < lower, then we have conflicting constraints. Consider:
    //
    // if (x < 0) {
    //   if (x > 0) {
    //     [Some code.]
    //   }
    // }
    //
    // In this case, the block is unreachable.
    if (newUpper < newLower) {
        // If both ranges can be NaN, the result can still be NaN.
        if (!lhs->canBeNaN() || !rhs->canBeNaN())
            *emptyRange = true;
        return nullptr;
    }

    bool newHasInt32LowerBound = lhs->hasInt32LowerBound_ || rhs->hasInt32LowerBound_;
    bool newHasInt32UpperBound = lhs->hasInt32UpperBound_ || rhs->hasInt32UpperBound_;

    FractionalPartFlag newCanHaveFractionalPart = FractionalPartFlag(lhs->canHaveFractionalPart_ &&
                                                                     rhs->canHaveFractionalPart_);
    NegativeZeroFlag newMayIncludeNegativeZero = NegativeZeroFlag(lhs->canBeNegativeZero_ &&
                                                                  rhs->canBeNegativeZero_);

    uint16_t newExponent = Min(lhs->max_exponent_, rhs->max_exponent_);

    // NaN is a special value which is neither greater than infinity or less than
    // negative infinity. When we intersect two ranges like [?, 0] and [0, ?], we
    // can end up thinking we have both a lower and upper bound, even though NaN
    // is still possible. In this case, just be conservative, since any case where
    // we can have NaN is not especially interesting.
    if (newHasInt32LowerBound && newHasInt32UpperBound && newExponent == IncludesInfinityAndNaN)
        return nullptr;

    // If one of the ranges has a fractional part and the other doesn't, it's
    // possible that we will have computed a newExponent that's more precise
    // than our newLower and newUpper. This is unusual, so we handle it here
    // instead of in optimize().
    //
    // For example, consider the range F[0,1.5]. Range analysis represents the
    // lower and upper bound as integers, so we'd actually have
    // F[0,2] (< pow(2, 0+1)). In this case, the exponent gives us a slightly
    // more precise upper bound than the integer upper bound.
    //
    // When intersecting such a range with an integer range, the fractional part
    // of the range is dropped. The max exponent of 0 remains valid, so the
    // upper bound needs to be adjusted to 1.
    //
    // When intersecting F[0,2] (< pow(2, 0+1)) with a range like F[2,4],
    // the naive intersection is I[2,2], but since the max exponent tells us
    // that the value is always less than 2, the intersection is actually empty.
    if (lhs->canHaveFractionalPart() != rhs->canHaveFractionalPart() ||
        (lhs->canHaveFractionalPart() &&
         newHasInt32LowerBound && newHasInt32UpperBound &&
         newLower == newUpper))
    {
        refineInt32BoundsByExponent(newExponent,
                                    &newLower, &newHasInt32LowerBound,
                                    &newUpper, &newHasInt32UpperBound);

        // If we're intersecting two ranges that don't overlap, this could also
        // push the bounds past each other, since the actual intersection is
        // the empty set.
        if (newLower > newUpper) {
            *emptyRange = true;
            return nullptr;
        }
    }

    return new(alloc) Range(newLower, newHasInt32LowerBound, newUpper, newHasInt32UpperBound,
                            newCanHaveFractionalPart,
                            newMayIncludeNegativeZero,
                            newExponent);
}

void
Range::unionWith(const Range* other)
{
    int32_t newLower = Min(lower_, other->lower_);
    int32_t newUpper = Max(upper_, other->upper_);

    bool newHasInt32LowerBound = hasInt32LowerBound_ && other->hasInt32LowerBound_;
    bool newHasInt32UpperBound = hasInt32UpperBound_ && other->hasInt32UpperBound_;

    FractionalPartFlag newCanHaveFractionalPart =
        FractionalPartFlag(canHaveFractionalPart_ ||
                           other->canHaveFractionalPart_);
    NegativeZeroFlag newMayIncludeNegativeZero = NegativeZeroFlag(canBeNegativeZero_ ||
                                                                  other->canBeNegativeZero_);

    uint16_t newExponent = Max(max_exponent_, other->max_exponent_);

    rawInitialize(newLower, newHasInt32LowerBound, newUpper, newHasInt32UpperBound,
                  newCanHaveFractionalPart,
                  newMayIncludeNegativeZero,
                  newExponent);
}

Range::Range(const MDefinition* def)
  : symbolicLower_(nullptr),
    symbolicUpper_(nullptr)
{
    if (const Range* other = def->range()) {
        // The instruction has range information; use it.
        *this = *other;

        // Simulate the effect of converting the value to its type.
        // Note: we cannot clamp here, since ranges aren't allowed to shrink
        // and truncation can increase range again. So doing wrapAround to
        // mimick a possible truncation.
        switch (def->type()) {
          case MIRType::Int32:
            // MToNumberInt32 cannot truncate. So we can safely clamp.
            if (def->isToNumberInt32())
                clampToInt32();
            else
                wrapAroundToInt32();
            break;
          case MIRType::Boolean:
            wrapAroundToBoolean();
            break;
          case MIRType::None:
            MOZ_CRASH("Asking for the range of an instruction with no value");
          default:
            break;
        }
    } else {
        // Otherwise just use type information. We can trust the type here
        // because we don't care what value the instruction actually produces,
        // but what value we might get after we get past the bailouts.
        switch (def->type()) {
          case MIRType::Int32:
            setInt32(JSVAL_INT_MIN, JSVAL_INT_MAX);
            break;
          case MIRType::Boolean:
            setInt32(0, 1);
            break;
          case MIRType::None:
            MOZ_CRASH("Asking for the range of an instruction with no value");
          default:
            setUnknown();
            break;
        }
    }

    // As a special case, MUrsh is permitted to claim a result type of
    // MIRType::Int32 while actually returning values in [0,UINT32_MAX] without
    // bailouts. If range analysis hasn't ruled out values in
    // (INT32_MAX,UINT32_MAX], set the range to be conservatively correct for
    // use as either a uint32 or an int32.
    if (!hasInt32UpperBound() &&
        def->isUrsh() &&
        def->toUrsh()->bailoutsDisabled() &&
        def->type() != MIRType::Int64)
    {
        lower_ = INT32_MIN;
    }

    assertInvariants();
}

static uint16_t
ExponentImpliedByDouble(double d)
{
    // Handle the special values.
    if (IsNaN(d))
        return Range::IncludesInfinityAndNaN;
    if (IsInfinite(d))
        return Range::IncludesInfinity;

    // Otherwise take the exponent part and clamp it at zero, since the Range
    // class doesn't track fractional ranges.
    return uint16_t(Max(int_fast16_t(0), ExponentComponent(d)));
}

void
Range::setDouble(double l, double h)
{
    MOZ_ASSERT(!(l > h));

    // Infer lower_, upper_, hasInt32LowerBound_, and hasInt32UpperBound_.
    if (l >= INT32_MIN && l <= INT32_MAX) {
        lower_ = int32_t(::floor(l));
        hasInt32LowerBound_ = true;
    } else if (l >= INT32_MAX) {
        lower_ = INT32_MAX;
        hasInt32LowerBound_ = true;
    } else {
        lower_ = INT32_MIN;
        hasInt32LowerBound_ = false;
    }
    if (h >= INT32_MIN && h <= INT32_MAX) {
        upper_ = int32_t(::ceil(h));
        hasInt32UpperBound_ = true;
    } else if (h <= INT32_MIN) {
        upper_ = INT32_MIN;
        hasInt32UpperBound_ = true;
    } else {
        upper_ = INT32_MAX;
        hasInt32UpperBound_ = false;
    }

    // Infer max_exponent_.
    uint16_t lExp = ExponentImpliedByDouble(l);
    uint16_t hExp = ExponentImpliedByDouble(h);
    max_exponent_ = Max(lExp, hExp);

    canHaveFractionalPart_ = ExcludesFractionalParts;
    canBeNegativeZero_ = ExcludesNegativeZero;

    // Infer the canHaveFractionalPart_ setting. We can have a
    // fractional part if the range crosses through the neighborhood of zero. We
    // won't have a fractional value if the value is always beyond the point at
    // which double precision can't represent fractional values.
    uint16_t minExp = Min(lExp, hExp);
    bool includesNegative = IsNaN(l) || l < 0;
    bool includesPositive = IsNaN(h) || h > 0;
    bool crossesZero = includesNegative && includesPositive;
    if (crossesZero || minExp < MaxTruncatableExponent)
        canHaveFractionalPart_ = IncludesFractionalParts;

    // Infer the canBeNegativeZero_ setting. We can have a negative zero if
    // either bound is zero.
    if (!(l > 0) && !(h < 0))
        canBeNegativeZero_ = IncludesNegativeZero;

    optimize();
}

void
Range::setDoubleSingleton(double d)
{
    setDouble(d, d);

    // The above setDouble call is for comparisons, and treats negative zero
    // as equal to zero. We're aiming for a minimum range, so we can clear the
    // negative zero flag if the value isn't actually negative zero.
    if (!IsNegativeZero(d))
        canBeNegativeZero_ = ExcludesNegativeZero;

    assertInvariants();
}

static inline bool
MissingAnyInt32Bounds(const Range* lhs, const Range* rhs)
{
    return !lhs->hasInt32Bounds() || !rhs->hasInt32Bounds();
}

Range*
Range::add(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    int64_t l = (int64_t) lhs->lower_ + (int64_t) rhs->lower_;
    if (!lhs->hasInt32LowerBound() || !rhs->hasInt32LowerBound())
        l = NoInt32LowerBound;

    int64_t h = (int64_t) lhs->upper_ + (int64_t) rhs->upper_;
    if (!lhs->hasInt32UpperBound() || !rhs->hasInt32UpperBound())
        h = NoInt32UpperBound;

    // The exponent is at most one greater than the greater of the operands'
    // exponents, except for NaN and infinity cases.
    uint16_t e = Max(lhs->max_exponent_, rhs->max_exponent_);
    if (e <= Range::MaxFiniteExponent)
        ++e;

    // Infinity + -Infinity is NaN.
    if (lhs->canBeInfiniteOrNaN() && rhs->canBeInfiniteOrNaN())
        e = Range::IncludesInfinityAndNaN;

    return new(alloc) Range(l, h,
                            FractionalPartFlag(lhs->canHaveFractionalPart() ||
                                               rhs->canHaveFractionalPart()),
                            NegativeZeroFlag(lhs->canBeNegativeZero() &&
                                             rhs->canBeNegativeZero()),
                            e);
}

Range*
Range::sub(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    int64_t l = (int64_t) lhs->lower_ - (int64_t) rhs->upper_;
    if (!lhs->hasInt32LowerBound() || !rhs->hasInt32UpperBound())
        l = NoInt32LowerBound;

    int64_t h = (int64_t) lhs->upper_ - (int64_t) rhs->lower_;
    if (!lhs->hasInt32UpperBound() || !rhs->hasInt32LowerBound())
        h = NoInt32UpperBound;

    // The exponent is at most one greater than the greater of the operands'
    // exponents, except for NaN and infinity cases.
    uint16_t e = Max(lhs->max_exponent_, rhs->max_exponent_);
    if (e <= Range::MaxFiniteExponent)
        ++e;

    // Infinity - Infinity is NaN.
    if (lhs->canBeInfiniteOrNaN() && rhs->canBeInfiniteOrNaN())
        e = Range::IncludesInfinityAndNaN;

    return new(alloc) Range(l, h,
                            FractionalPartFlag(lhs->canHaveFractionalPart() ||
                                               rhs->canHaveFractionalPart()),
                            NegativeZeroFlag(lhs->canBeNegativeZero() &&
                                             rhs->canBeZero()),
                            e);
}

Range*
Range::and_(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    MOZ_ASSERT(lhs->isInt32());
    MOZ_ASSERT(rhs->isInt32());

    // If both numbers can be negative, result can be negative in the whole range
    if (lhs->lower() < 0 && rhs->lower() < 0)
        return Range::NewInt32Range(alloc, INT32_MIN, Max(lhs->upper(), rhs->upper()));

    // Only one of both numbers can be negative.
    // - result can't be negative
    // - Upper bound is minimum of both upper range,
    int32_t lower = 0;
    int32_t upper = Min(lhs->upper(), rhs->upper());

    // EXCEPT when upper bound of non negative number is max value,
    // because negative value can return the whole max value.
    // -1 & 5 = 5
    if (lhs->lower() < 0)
       upper = rhs->upper();
    if (rhs->lower() < 0)
        upper = lhs->upper();

    return Range::NewInt32Range(alloc, lower, upper);
}

Range*
Range::or_(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    MOZ_ASSERT(lhs->isInt32());
    MOZ_ASSERT(rhs->isInt32());
    // When one operand is always 0 or always -1, it's a special case where we
    // can compute a fully precise result. Handling these up front also
    // protects the code below from calling CountLeadingZeroes32 with a zero
    // operand or from shifting an int32_t by 32.
    if (lhs->lower() == lhs->upper()) {
        if (lhs->lower() == 0)
            return new(alloc) Range(*rhs);
        if (lhs->lower() == -1)
            return new(alloc) Range(*lhs);
    }
    if (rhs->lower() == rhs->upper()) {
        if (rhs->lower() == 0)
            return new(alloc) Range(*lhs);
        if (rhs->lower() == -1)
            return new(alloc) Range(*rhs);
    }

    // The code below uses CountLeadingZeroes32, which has undefined behavior
    // if its operand is 0. We rely on the code above to protect it.
    MOZ_ASSERT_IF(lhs->lower() >= 0, lhs->upper() != 0);
    MOZ_ASSERT_IF(rhs->lower() >= 0, rhs->upper() != 0);
    MOZ_ASSERT_IF(lhs->upper() < 0, lhs->lower() != -1);
    MOZ_ASSERT_IF(rhs->upper() < 0, rhs->lower() != -1);

    int32_t lower = INT32_MIN;
    int32_t upper = INT32_MAX;

    if (lhs->lower() >= 0 && rhs->lower() >= 0) {
        // Both operands are non-negative, so the result won't be less than either.
        lower = Max(lhs->lower(), rhs->lower());
        // The result will have leading zeros where both operands have leading zeros.
        // CountLeadingZeroes32 of a non-negative int32 will at least be 1 to account
        // for the bit of sign.
        upper = int32_t(UINT32_MAX >> Min(CountLeadingZeroes32(lhs->upper()),
                                          CountLeadingZeroes32(rhs->upper())));
    } else {
        // The result will have leading ones where either operand has leading ones.
        if (lhs->upper() < 0) {
            unsigned leadingOnes = CountLeadingZeroes32(~lhs->lower());
            lower = Max(lower, ~int32_t(UINT32_MAX >> leadingOnes));
            upper = -1;
        }
        if (rhs->upper() < 0) {
            unsigned leadingOnes = CountLeadingZeroes32(~rhs->lower());
            lower = Max(lower, ~int32_t(UINT32_MAX >> leadingOnes));
            upper = -1;
        }
    }

    return Range::NewInt32Range(alloc, lower, upper);
}

Range*
Range::xor_(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    MOZ_ASSERT(lhs->isInt32());
    MOZ_ASSERT(rhs->isInt32());
    int32_t lhsLower = lhs->lower();
    int32_t lhsUpper = lhs->upper();
    int32_t rhsLower = rhs->lower();
    int32_t rhsUpper = rhs->upper();
    bool invertAfter = false;

    // If either operand is negative, bitwise-negate it, and arrange to negate
    // the result; ~((~x)^y) == x^y. If both are negative the negations on the
    // result cancel each other out; effectively this is (~x)^(~y) == x^y.
    // These transformations reduce the number of cases we have to handle below.
    if (lhsUpper < 0) {
        lhsLower = ~lhsLower;
        lhsUpper = ~lhsUpper;
        Swap(lhsLower, lhsUpper);
        invertAfter = !invertAfter;
    }
    if (rhsUpper < 0) {
        rhsLower = ~rhsLower;
        rhsUpper = ~rhsUpper;
        Swap(rhsLower, rhsUpper);
        invertAfter = !invertAfter;
    }

    // Handle cases where lhs or rhs is always zero specially, because they're
    // easy cases where we can be perfectly precise, and because it protects the
    // CountLeadingZeroes32 calls below from seeing 0 operands, which would be
    // undefined behavior.
    int32_t lower = INT32_MIN;
    int32_t upper = INT32_MAX;
    if (lhsLower == 0 && lhsUpper == 0) {
        upper = rhsUpper;
        lower = rhsLower;
    } else if (rhsLower == 0 && rhsUpper == 0) {
        upper = lhsUpper;
        lower = lhsLower;
    } else if (lhsLower >= 0 && rhsLower >= 0) {
        // Both operands are non-negative. The result will be non-negative.
        lower = 0;
        // To compute the upper value, take each operand's upper value and
        // set all bits that don't correspond to leading zero bits in the
        // other to one. For each one, this gives an upper bound for the
        // result, so we can take the minimum between the two.
        unsigned lhsLeadingZeros = CountLeadingZeroes32(lhsUpper);
        unsigned rhsLeadingZeros = CountLeadingZeroes32(rhsUpper);
        upper = Min(rhsUpper | int32_t(UINT32_MAX >> lhsLeadingZeros),
                    lhsUpper | int32_t(UINT32_MAX >> rhsLeadingZeros));
    }

    // If we bitwise-negated one (but not both) of the operands above, apply the
    // bitwise-negate to the result, completing ~((~x)^y) == x^y.
    if (invertAfter) {
        lower = ~lower;
        upper = ~upper;
        Swap(lower, upper);
    }

    return Range::NewInt32Range(alloc, lower, upper);
}

Range*
Range::not_(TempAllocator& alloc, const Range* op)
{
    MOZ_ASSERT(op->isInt32());
    return Range::NewInt32Range(alloc, ~op->upper(), ~op->lower());
}

Range*
Range::mul(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    FractionalPartFlag newCanHaveFractionalPart = FractionalPartFlag(lhs->canHaveFractionalPart_ ||
                                                                     rhs->canHaveFractionalPart_);

    NegativeZeroFlag newMayIncludeNegativeZero =
        NegativeZeroFlag((lhs->canHaveSignBitSet() && rhs->canBeFiniteNonNegative()) ||
                         (rhs->canHaveSignBitSet() && lhs->canBeFiniteNonNegative()));

    uint16_t exponent;
    if (!lhs->canBeInfiniteOrNaN() && !rhs->canBeInfiniteOrNaN()) {
        // Two finite values.
        exponent = lhs->numBits() + rhs->numBits() - 1;
        if (exponent > Range::MaxFiniteExponent)
            exponent = Range::IncludesInfinity;
    } else if (!lhs->canBeNaN() &&
               !rhs->canBeNaN() &&
               !(lhs->canBeZero() && rhs->canBeInfiniteOrNaN()) &&
               !(rhs->canBeZero() && lhs->canBeInfiniteOrNaN()))
    {
        // Two values that multiplied together won't produce a NaN.
        exponent = Range::IncludesInfinity;
    } else {
        // Could be anything.
        exponent = Range::IncludesInfinityAndNaN;
    }

    if (MissingAnyInt32Bounds(lhs, rhs))
        return new(alloc) Range(NoInt32LowerBound, NoInt32UpperBound,
                                newCanHaveFractionalPart,
                                newMayIncludeNegativeZero,
                                exponent);
    int64_t a = (int64_t)lhs->lower() * (int64_t)rhs->lower();
    int64_t b = (int64_t)lhs->lower() * (int64_t)rhs->upper();
    int64_t c = (int64_t)lhs->upper() * (int64_t)rhs->lower();
    int64_t d = (int64_t)lhs->upper() * (int64_t)rhs->upper();
    return new(alloc) Range(
        Min( Min(a, b), Min(c, d) ),
        Max( Max(a, b), Max(c, d) ),
        newCanHaveFractionalPart,
        newMayIncludeNegativeZero,
        exponent);
}

Range*
Range::lsh(TempAllocator& alloc, const Range* lhs, int32_t c)
{
    MOZ_ASSERT(lhs->isInt32());
    int32_t shift = c & 0x1f;

    // If the shift doesn't loose bits or shift bits into the sign bit, we
    // can simply compute the correct range by shifting.
    if ((int32_t)((uint32_t)lhs->lower() << shift << 1 >> shift >> 1) == lhs->lower() &&
        (int32_t)((uint32_t)lhs->upper() << shift << 1 >> shift >> 1) == lhs->upper())
    {
        return Range::NewInt32Range(alloc,
            uint32_t(lhs->lower()) << shift,
            uint32_t(lhs->upper()) << shift);
    }

    return Range::NewInt32Range(alloc, INT32_MIN, INT32_MAX);
}

Range*
Range::rsh(TempAllocator& alloc, const Range* lhs, int32_t c)
{
    MOZ_ASSERT(lhs->isInt32());
    int32_t shift = c & 0x1f;
    return Range::NewInt32Range(alloc,
        lhs->lower() >> shift,
        lhs->upper() >> shift);
}

Range*
Range::ursh(TempAllocator& alloc, const Range* lhs, int32_t c)
{
    // ursh's left operand is uint32, not int32, but for range analysis we
    // currently approximate it as int32. We assume here that the range has
    // already been adjusted accordingly by our callers.
    MOZ_ASSERT(lhs->isInt32());

    int32_t shift = c & 0x1f;

    // If the value is always non-negative or always negative, we can simply
    // compute the correct range by shifting.
    if (lhs->isFiniteNonNegative() || lhs->isFiniteNegative()) {
        return Range::NewUInt32Range(alloc,
            uint32_t(lhs->lower()) >> shift,
            uint32_t(lhs->upper()) >> shift);
    }

    // Otherwise return the most general range after the shift.
    return Range::NewUInt32Range(alloc, 0, UINT32_MAX >> shift);
}

Range*
Range::lsh(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    MOZ_ASSERT(lhs->isInt32());
    MOZ_ASSERT(rhs->isInt32());
    return Range::NewInt32Range(alloc, INT32_MIN, INT32_MAX);
}

Range*
Range::rsh(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    MOZ_ASSERT(lhs->isInt32());
    MOZ_ASSERT(rhs->isInt32());

    // Canonicalize the shift range to 0 to 31.
    int32_t shiftLower = rhs->lower();
    int32_t shiftUpper = rhs->upper();
    if ((int64_t(shiftUpper) - int64_t(shiftLower)) >= 31) {
        shiftLower = 0;
        shiftUpper = 31;
    } else {
        shiftLower &= 0x1f;
        shiftUpper &= 0x1f;
        if (shiftLower > shiftUpper) {
            shiftLower = 0;
            shiftUpper = 31;
        }
    }
    MOZ_ASSERT(shiftLower >= 0 && shiftUpper <= 31);

    // The lhs bounds are signed, thus the minimum is either the lower bound
    // shift by the smallest shift if negative or the lower bound shifted by the
    // biggest shift otherwise.  And the opposite for the maximum.
    int32_t lhsLower = lhs->lower();
    int32_t min = lhsLower < 0 ? lhsLower >> shiftLower : lhsLower >> shiftUpper;
    int32_t lhsUpper = lhs->upper();
    int32_t max = lhsUpper >= 0 ? lhsUpper >> shiftLower : lhsUpper >> shiftUpper;

    return Range::NewInt32Range(alloc, min, max);
}

Range*
Range::ursh(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    // ursh's left operand is uint32, not int32, but for range analysis we
    // currently approximate it as int32. We assume here that the range has
    // already been adjusted accordingly by our callers.
    MOZ_ASSERT(lhs->isInt32());
    MOZ_ASSERT(rhs->isInt32());
    return Range::NewUInt32Range(alloc, 0, lhs->isFiniteNonNegative() ? lhs->upper() : UINT32_MAX);
}

Range*
Range::abs(TempAllocator& alloc, const Range* op)
{
    int32_t l = op->lower_;
    int32_t u = op->upper_;
    FractionalPartFlag canHaveFractionalPart = op->canHaveFractionalPart_;

    // Abs never produces a negative zero.
    NegativeZeroFlag canBeNegativeZero = ExcludesNegativeZero;

    return new(alloc) Range(Max(Max(int32_t(0), l), u == INT32_MIN ? INT32_MAX : -u),
                            true,
                            Max(Max(int32_t(0), u), l == INT32_MIN ? INT32_MAX : -l),
                            op->hasInt32Bounds() && l != INT32_MIN,
                            canHaveFractionalPart,
                            canBeNegativeZero,
                            op->max_exponent_);
}

Range*
Range::min(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    // If either operand is NaN, the result is NaN.
    if (lhs->canBeNaN() || rhs->canBeNaN())
        return nullptr;

    FractionalPartFlag newCanHaveFractionalPart = FractionalPartFlag(lhs->canHaveFractionalPart_ ||
                                                                     rhs->canHaveFractionalPart_);
    NegativeZeroFlag newMayIncludeNegativeZero = NegativeZeroFlag(lhs->canBeNegativeZero_ ||
                                                                  rhs->canBeNegativeZero_);

    return new(alloc) Range(Min(lhs->lower_, rhs->lower_),
                            lhs->hasInt32LowerBound_ && rhs->hasInt32LowerBound_,
                            Min(lhs->upper_, rhs->upper_),
                            lhs->hasInt32UpperBound_ || rhs->hasInt32UpperBound_,
                            newCanHaveFractionalPart,
                            newMayIncludeNegativeZero,
                            Max(lhs->max_exponent_, rhs->max_exponent_));
}

Range*
Range::max(TempAllocator& alloc, const Range* lhs, const Range* rhs)
{
    // If either operand is NaN, the result is NaN.
    if (lhs->canBeNaN() || rhs->canBeNaN())
        return nullptr;

    FractionalPartFlag newCanHaveFractionalPart = FractionalPartFlag(lhs->canHaveFractionalPart_ ||
                                                                     rhs->canHaveFractionalPart_);
    NegativeZeroFlag newMayIncludeNegativeZero = NegativeZeroFlag(lhs->canBeNegativeZero_ ||
                                                                  rhs->canBeNegativeZero_);

    return new(alloc) Range(Max(lhs->lower_, rhs->lower_),
                            lhs->hasInt32LowerBound_ || rhs->hasInt32LowerBound_,
                            Max(lhs->upper_, rhs->upper_),
                            lhs->hasInt32UpperBound_ && rhs->hasInt32UpperBound_,
                            newCanHaveFractionalPart,
                            newMayIncludeNegativeZero,
                            Max(lhs->max_exponent_, rhs->max_exponent_));
}

Range*
Range::floor(TempAllocator& alloc, const Range* op)
{
    Range* copy = new(alloc) Range(*op);
    // Decrement lower bound of copy range if op have a factional part and lower
    // bound is Int32 defined. Also we avoid to decrement when op have a
    // fractional part but lower_ >= JSVAL_INT_MAX.
    if (op->canHaveFractionalPart() && op->hasInt32LowerBound())
        copy->setLowerInit(int64_t(copy->lower_) - 1);

    // Also refine max_exponent_ because floor may have decremented int value
    // If we've got int32 defined bounds, just deduce it using defined bounds.
    // But, if we don't have those, value's max_exponent_ may have changed.
    // Because we're looking to maintain an over estimation, if we can,
    // we increment it.
    if(copy->hasInt32Bounds())
        copy->max_exponent_ = copy->exponentImpliedByInt32Bounds();
    else if(copy->max_exponent_ < MaxFiniteExponent)
        copy->max_exponent_++;

    copy->canHaveFractionalPart_ = ExcludesFractionalParts;
    copy->assertInvariants();
    return copy;
}

Range*
Range::ceil(TempAllocator& alloc, const Range* op)
{
    Range* copy = new(alloc) Range(*op);

    // We need to refine max_exponent_ because ceil may have incremented the int value.
    // If we have got int32 bounds defined, just deduce it using the defined bounds.
    // Else we can just increment its value,
    // as we are looking to maintain an over estimation.
    if (copy->hasInt32Bounds())
        copy->max_exponent_ = copy->exponentImpliedByInt32Bounds();
    else if (copy->max_exponent_ < MaxFiniteExponent)
        copy->max_exponent_++;

    copy->canHaveFractionalPart_ = ExcludesFractionalParts;
    copy->assertInvariants();
    return copy;
}

Range*
Range::sign(TempAllocator& alloc, const Range* op)
{
    if (op->canBeNaN())
        return nullptr;

    return new(alloc) Range(Max(Min(op->lower_, 1), -1),
                            Max(Min(op->upper_, 1), -1),
                            Range::ExcludesFractionalParts,
                            NegativeZeroFlag(op->canBeNegativeZero()),
                            0);
}

Range*
Range::NaNToZero(TempAllocator& alloc, const Range *op)
{
    Range* copy = new(alloc) Range(*op);
    if (copy->canBeNaN()) {
        copy->max_exponent_ = Range::IncludesInfinity;
        if (!copy->canBeZero()) {
            Range zero;
            zero.setDoubleSingleton(0);
            copy->unionWith(&zero);
        }
    }
    copy->refineToExcludeNegativeZero();
    return copy;
}

bool
Range::negativeZeroMul(const Range* lhs, const Range* rhs)
{
    // The result can only be negative zero if both sides are finite and they
    // have differing signs.
    return (lhs->canHaveSignBitSet() && rhs->canBeFiniteNonNegative()) ||
           (rhs->canHaveSignBitSet() && lhs->canBeFiniteNonNegative());
}

bool
Range::update(const Range* other)
{
    bool changed =
        lower_ != other->lower_ ||
        hasInt32LowerBound_ != other->hasInt32LowerBound_ ||
        upper_ != other->upper_ ||
        hasInt32UpperBound_ != other->hasInt32UpperBound_ ||
        canHaveFractionalPart_ != other->canHaveFractionalPart_ ||
        canBeNegativeZero_ != other->canBeNegativeZero_ ||
        max_exponent_ != other->max_exponent_;
    if (changed) {
        lower_ = other->lower_;
        hasInt32LowerBound_ = other->hasInt32LowerBound_;
        upper_ = other->upper_;
        hasInt32UpperBound_ = other->hasInt32UpperBound_;
        canHaveFractionalPart_ = other->canHaveFractionalPart_;
        canBeNegativeZero_ = other->canBeNegativeZero_;
        max_exponent_ = other->max_exponent_;
        assertInvariants();
    }

    return changed;
}

///////////////////////////////////////////////////////////////////////////////
// Range Computation for MIR Nodes
///////////////////////////////////////////////////////////////////////////////

void
MPhi::computeRange(TempAllocator& alloc)
{
    if (type() != MIRType::Int32 && type() != MIRType::Double)
        return;

    Range* range = nullptr;
    for (size_t i = 0, e = numOperands(); i < e; i++) {
        if (getOperand(i)->block()->unreachable()) {
            JitSpew(JitSpew_Range, "Ignoring unreachable input %d", getOperand(i)->id());
            continue;
        }

        // Peek at the pre-bailout range so we can take a short-cut; if any of
        // the operands has an unknown range, this phi has an unknown range.
        if (!getOperand(i)->range())
            return;

        Range input(getOperand(i));

        if (range)
            range->unionWith(&input);
        else
            range = new(alloc) Range(input);
    }

    setRange(range);
}

void
MBeta::computeRange(TempAllocator& alloc)
{
    bool emptyRange = false;

    Range opRange(getOperand(0));
    Range* range = Range::intersect(alloc, &opRange, comparison_, &emptyRange);
    if (emptyRange) {
        JitSpew(JitSpew_Range, "Marking block for inst %d unreachable", id());
        block()->setUnreachableUnchecked();
    } else {
        setRange(range);
    }
}

void
MConstant::computeRange(TempAllocator& alloc)
{
    if (isTypeRepresentableAsDouble()) {
        double d = numberToDouble();
        setRange(Range::NewDoubleSingletonRange(alloc, d));
    } else if (type() == MIRType::Boolean) {
        bool b = toBoolean();
        setRange(Range::NewInt32Range(alloc, b, b));
    }
}

void
MCharCodeAt::computeRange(TempAllocator& alloc)
{
    // ECMA 262 says that the integer will be non-negative and at most 65535.
    setRange(Range::NewInt32Range(alloc, 0, 65535));
}

void
MClampToUint8::computeRange(TempAllocator& alloc)
{
    setRange(Range::NewUInt32Range(alloc, 0, 255));
}

void
MBitAnd::computeRange(TempAllocator& alloc)
{
    if (specialization_ == MIRType::Int64)
        return;

    Range left(getOperand(0));
    Range right(getOperand(1));
    left.wrapAroundToInt32();
    right.wrapAroundToInt32();

    setRange(Range::and_(alloc, &left, &right));
}

void
MBitOr::computeRange(TempAllocator& alloc)
{
    if (specialization_ == MIRType::Int64)
        return;

    Range left(getOperand(0));
    Range right(getOperand(1));
    left.wrapAroundToInt32();
    right.wrapAroundToInt32();

    setRange(Range::or_(alloc, &left, &right));
}

void
MBitXor::computeRange(TempAllocator& alloc)
{
    if (specialization_ == MIRType::Int64)
        return;

    Range left(getOperand(0));
    Range right(getOperand(1));
    left.wrapAroundToInt32();
    right.wrapAroundToInt32();

    setRange(Range::xor_(alloc, &left, &right));
}

void
MBitNot::computeRange(TempAllocator& alloc)
{
    Range op(getOperand(0));
    op.wrapAroundToInt32();

    setRange(Range::not_(alloc, &op));
}

void
MLsh::computeRange(TempAllocator& alloc)
{
    if (specialization_ == MIRType::Int64)
        return;

    Range left(getOperand(0));
    Range right(getOperand(1));
    left.wrapAroundToInt32();

    MConstant* rhsConst = getOperand(1)->maybeConstantValue();
    if (rhsConst && rhsConst->type() == MIRType::Int32) {
        int32_t c = rhsConst->toInt32();
        setRange(Range::lsh(alloc, &left, c));
        return;
    }

    right.wrapAroundToShiftCount();
    setRange(Range::lsh(alloc, &left, &right));
}

void
MRsh::computeRange(TempAllocator& alloc)
{
    if (specialization_ == MIRType::Int64)
        return;

    Range left(getOperand(0));
    Range right(getOperand(1));
    left.wrapAroundToInt32();

    MConstant* rhsConst = getOperand(1)->maybeConstantValue();
    if (rhsConst && rhsConst->type() == MIRType::Int32) {
        int32_t c = rhsConst->toInt32();
        setRange(Range::rsh(alloc, &left, c));
        return;
    }

    right.wrapAroundToShiftCount();
    setRange(Range::rsh(alloc, &left, &right));
}

void
MUrsh::computeRange(TempAllocator& alloc)
{
    if (specialization_ == MIRType::Int64)
        return;

    Range left(getOperand(0));
    Range right(getOperand(1));

    // ursh can be thought of as converting its left operand to uint32, or it
    // can be thought of as converting its left operand to int32, and then
    // reinterpreting the int32 bits as a uint32 value. Both approaches yield
    // the same result. Since we lack support for full uint32 ranges, we use
    // the second interpretation, though it does cause us to be conservative.
    left.wrapAroundToInt32();
    right.wrapAroundToShiftCount();

    MConstant* rhsConst = getOperand(1)->maybeConstantValue();
    if (rhsConst && rhsConst->type() == MIRType::Int32) {
        int32_t c = rhsConst->toInt32();
        setRange(Range::ursh(alloc, &left, c));
    } else {
        setRange(Range::ursh(alloc, &left, &right));
    }

    MOZ_ASSERT(range()->lower() >= 0);
}

void
MAbs::computeRange(TempAllocator& alloc)
{
    if (specialization_ != MIRType::Int32 && specialization_ != MIRType::Double)
        return;

    Range other(getOperand(0));
    Range* next = Range::abs(alloc, &other);
    if (implicitTruncate_)
        next->wrapAroundToInt32();
    setRange(next);
}

void
MFloor::computeRange(TempAllocator& alloc)
{
    Range other(getOperand(0));
    setRange(Range::floor(alloc, &other));
}

void
MCeil::computeRange(TempAllocator& alloc)
{
    Range other(getOperand(0));
    setRange(Range::ceil(alloc, &other));
}

void
MClz::computeRange(TempAllocator& alloc)
{
    if (type() != MIRType::Int32)
        return;
    setRange(Range::NewUInt32Range(alloc, 0, 32));
}

void
MCtz::computeRange(TempAllocator& alloc)
{
    if (type() != MIRType::Int32)
        return;
    setRange(Range::NewUInt32Range(alloc, 0, 32));
}

void
MPopcnt::computeRange(TempAllocator& alloc)
{
    if (type() != MIRType::Int32)
        return;
    setRange(Range::NewUInt32Range(alloc, 0, 32));
}

void
MMinMax::computeRange(TempAllocator& alloc)
{
    if (specialization_ != MIRType::Int32 && specialization_ != MIRType::Double)
        return;

    Range left(getOperand(0));
    Range right(getOperand(1));
    setRange(isMax() ? Range::max(alloc, &left, &right) : Range::min(alloc, &left, &right));
}

void
MAdd::computeRange(TempAllocator& alloc)
{
    if (specialization() != MIRType::Int32 && specialization() != MIRType::Double)
        return;
    Range left(getOperand(0));
    Range right(getOperand(1));
    Range* next = Range::add(alloc, &left, &right);
    if (isTruncated())
        next->wrapAroundToInt32();
    setRange(next);
}

void
MSub::computeRange(TempAllocator& alloc)
{
    if (specialization() != MIRType::Int32 && specialization() != MIRType::Double)
        return;
    Range left(getOperand(0));
    Range right(getOperand(1));
    Range* next = Range::sub(alloc, &left, &right);
    if (isTruncated())
        next->wrapAroundToInt32();
    setRange(next);
}

void
MMul::computeRange(TempAllocator& alloc)
{
    if (specialization() != MIRType::Int32 && specialization() != MIRType::Double)
        return;
    Range left(getOperand(0));
    Range right(getOperand(1));
    if (canBeNegativeZero())
        canBeNegativeZero_ = Range::negativeZeroMul(&left, &right);
    Range* next = Range::mul(alloc, &left, &right);
    if (!next->canBeNegativeZero())
        canBeNegativeZero_ = false;
    // Truncated multiplications could overflow in both directions
    if (isTruncated())
        next->wrapAroundToInt32();
    setRange(next);
}

void
MMod::computeRange(TempAllocator& alloc)
{
    if (specialization() != MIRType::Int32 && specialization() != MIRType::Double)
        return;
    Range lhs(getOperand(0));
    Range rhs(getOperand(1));

    // If either operand is a NaN, the result is NaN. This also conservatively
    // handles Infinity cases.
    if (!lhs.hasInt32Bounds() || !rhs.hasInt32Bounds())
        return;

    // If RHS can be zero, the result can be NaN.
    if (rhs.lower() <= 0 && rhs.upper() >= 0)
        return;

    // If both operands are non-negative integers, we can optimize this to an
    // unsigned mod.
    if (specialization() == MIRType::Int32 && rhs.lower() > 0) {
        bool hasDoubles = lhs.lower() < 0 || lhs.canHaveFractionalPart() ||
            rhs.canHaveFractionalPart();
        // It is not possible to check that lhs.lower() >= 0, since the range
        // of a ursh with rhs a 0 constant is wrapped around the int32 range in
        // Range::Range(). However, IsUint32Type() will only return true for
        // nodes that lie in the range [0, UINT32_MAX].
        bool hasUint32s = IsUint32Type(getOperand(0)) &&
            getOperand(1)->type() == MIRType::Int32 &&
            (IsUint32Type(getOperand(1)) || getOperand(1)->isConstant());
        if (!hasDoubles || hasUint32s)
            unsigned_ = true;
    }

    // For unsigned mod, we have to convert both operands to unsigned.
    // Note that we handled the case of a zero rhs above.
    if (unsigned_) {
        // The result of an unsigned mod will never be unsigned-greater than
        // either operand.
        uint32_t lhsBound = Max<uint32_t>(lhs.lower(), lhs.upper());
        uint32_t rhsBound = Max<uint32_t>(rhs.lower(), rhs.upper());

        // If either range crosses through -1 as a signed value, it could be
        // the maximum unsigned value when interpreted as unsigned. If the range
        // doesn't include -1, then the simple max value we computed above is
        // correct.
        if (lhs.lower() <= -1 && lhs.upper() >= -1)
            lhsBound = UINT32_MAX;
        if (rhs.lower() <= -1 && rhs.upper() >= -1)
            rhsBound = UINT32_MAX;

        // The result will never be equal to the rhs, and we shouldn't have
        // any rounding to worry about.
        MOZ_ASSERT(!lhs.canHaveFractionalPart() && !rhs.canHaveFractionalPart());
        --rhsBound;

        // This gives us two upper bounds, so we can take the best one.
        setRange(Range::NewUInt32Range(alloc, 0, Min(lhsBound, rhsBound)));
        return;
    }

    // Math.abs(lhs % rhs) == Math.abs(lhs) % Math.abs(rhs).
    // First, the absolute value of the result will always be less than the
    // absolute value of rhs. (And if rhs is zero, the result is NaN).
    int64_t a = Abs<int64_t>(rhs.lower());
    int64_t b = Abs<int64_t>(rhs.upper());
    if (a == 0 && b == 0)
        return;
    int64_t rhsAbsBound = Max(a, b);

    // If the value is known to be integer, less-than abs(rhs) is equivalent
    // to less-than-or-equal abs(rhs)-1. This is important for being able to
    // say that the result of x%256 is an 8-bit unsigned number.
    if (!lhs.canHaveFractionalPart() && !rhs.canHaveFractionalPart())
        --rhsAbsBound;

    // Next, the absolute value of the result will never be greater than the
    // absolute value of lhs.
    int64_t lhsAbsBound = Max(Abs<int64_t>(lhs.lower()), Abs<int64_t>(lhs.upper()));

    // This gives us two upper bounds, so we can take the best one.
    int64_t absBound = Min(lhsAbsBound, rhsAbsBound);

    // Now consider the sign of the result.
    // If lhs is non-negative, the result will be non-negative.
    // If lhs is non-positive, the result will be non-positive.
    int64_t lower = lhs.lower() >= 0 ? 0 : -absBound;
    int64_t upper = lhs.upper() <= 0 ? 0 : absBound;

    Range::FractionalPartFlag newCanHaveFractionalPart =
        Range::FractionalPartFlag(lhs.canHaveFractionalPart() ||
                                  rhs.canHaveFractionalPart());

    // If the lhs can have the sign bit set and we can return a zero, it'll be a
    // negative zero.
    Range::NegativeZeroFlag newMayIncludeNegativeZero =
        Range::NegativeZeroFlag(lhs.canHaveSignBitSet());

    setRange(new(alloc) Range(lower, upper,
                              newCanHaveFractionalPart,
                              newMayIncludeNegativeZero,
                              Min(lhs.exponent(), rhs.exponent())));
}

void
MDiv::computeRange(TempAllocator& alloc)
{
    if (specialization() != MIRType::Int32 && specialization() != MIRType::Double)
        return;
    Range lhs(getOperand(0));
    Range rhs(getOperand(1));

    // If either operand is a NaN, the result is NaN. This also conservatively
    // handles Infinity cases.
    if (!lhs.hasInt32Bounds() || !rhs.hasInt32Bounds())
        return;

    // Something simple for now: When dividing by a positive rhs, the result
    // won't be further from zero than lhs.
    if (lhs.lower() >= 0 && rhs.lower() >= 1) {
        setRange(new(alloc) Range(0, lhs.upper(),
                                  Range::IncludesFractionalParts,
                                  Range::IncludesNegativeZero,
                                  lhs.exponent()));
    } else if (unsigned_ && rhs.lower() >= 1) {
        // We shouldn't set the unsigned flag if the inputs can have
        // fractional parts.
        MOZ_ASSERT(!lhs.canHaveFractionalPart() && !rhs.canHaveFractionalPart());
        // We shouldn't set the unsigned flag if the inputs can be
        // negative zero.
        MOZ_ASSERT(!lhs.canBeNegativeZero() && !rhs.canBeNegativeZero());
        // Unsigned division by a non-zero rhs will return a uint32 value.
        setRange(Range::NewUInt32Range(alloc, 0, UINT32_MAX));
    }
}

void
MSqrt::computeRange(TempAllocator& alloc)
{
    Range input(getOperand(0));

    // If either operand is a NaN, the result is NaN. This also conservatively
    // handles Infinity cases.
    if (!input.hasInt32Bounds())
        return;

    // Sqrt of a negative non-zero value is NaN.
    if (input.lower() < 0)
        return;

    // Something simple for now: When taking the sqrt of a positive value, the
    // result won't be further from zero than the input.
    // And, sqrt of an integer may have a fractional part.
    setRange(new(alloc) Range(0, input.upper(),
                              Range::IncludesFractionalParts,
                              input.canBeNegativeZero(),
                              input.exponent()));
}

void
MToDouble::computeRange(TempAllocator& alloc)
{
    setRange(new(alloc) Range(getOperand(0)));
}

void
MToFloat32::computeRange(TempAllocator& alloc)
{
}

void
MTruncateToInt32::computeRange(TempAllocator& alloc)
{
    Range* output = new(alloc) Range(getOperand(0));
    output->wrapAroundToInt32();
    setRange(output);
}

void
MToNumberInt32::computeRange(TempAllocator& alloc)
{
    // No clamping since this computes the range *before* bailouts.
    setRange(new(alloc) Range(getOperand(0)));
}

void
MLimitedTruncate::computeRange(TempAllocator& alloc)
{
    Range* output = new(alloc) Range(input());
    setRange(output);
}

void
MFilterTypeSet::computeRange(TempAllocator& alloc)
{
    setRange(new(alloc) Range(getOperand(0)));
}

static Range*
GetTypedArrayRange(TempAllocator& alloc, Scalar::Type type)
{
    switch (type) {
      case Scalar::Uint8Clamped:
      case Scalar::Uint8:
        return Range::NewUInt32Range(alloc, 0, UINT8_MAX);
      case Scalar::Uint16:
        return Range::NewUInt32Range(alloc, 0, UINT16_MAX);
      case Scalar::Uint32:
        return Range::NewUInt32Range(alloc, 0, UINT32_MAX);

      case Scalar::Int8:
        return Range::NewInt32Range(alloc, INT8_MIN, INT8_MAX);
      case Scalar::Int16:
        return Range::NewInt32Range(alloc, INT16_MIN, INT16_MAX);
      case Scalar::Int32:
        return Range::NewInt32Range(alloc, INT32_MIN, INT32_MAX);

      case Scalar::Int64:
      case Scalar::Float32:
      case Scalar::Float64:
      case Scalar::Float32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
      case Scalar::Int32x4:
      case Scalar::MaxTypedArrayViewType:
        break;
    }
    return nullptr;
}

void
MLoadUnboxedScalar::computeRange(TempAllocator& alloc)
{
    // We have an Int32 type and if this is a UInt32 load it may produce a value
    // outside of our range, but we have a bailout to handle those cases.
    setRange(GetTypedArrayRange(alloc, readType()));
}

void
MArrayLength::computeRange(TempAllocator& alloc)
{
    // Array lengths can go up to UINT32_MAX, but we only create MArrayLength
    // nodes when the value is known to be int32 (see the
    // OBJECT_FLAG_LENGTH_OVERFLOW flag).
    setRange(Range::NewUInt32Range(alloc, 0, INT32_MAX));
}

void
MInitializedLength::computeRange(TempAllocator& alloc)
{
    setRange(Range::NewUInt32Range(alloc, 0, NativeObject::MAX_DENSE_ELEMENTS_COUNT));
}

void
MTypedArrayLength::computeRange(TempAllocator& alloc)
{
    setRange(Range::NewUInt32Range(alloc, 0, INT32_MAX));
}

void
MStringLength::computeRange(TempAllocator& alloc)
{
    static_assert(JSString::MAX_LENGTH <= UINT32_MAX,
                  "NewUInt32Range requires a uint32 value");
    setRange(Range::NewUInt32Range(alloc, 0, JSString::MAX_LENGTH));
}

void
MArgumentsLength::computeRange(TempAllocator& alloc)
{
    // This is is a conservative upper bound on what |TooManyActualArguments|
    // checks.  If exceeded, Ion will not be entered in the first place.
    static_assert(ARGS_LENGTH_MAX <= UINT32_MAX,
                  "NewUInt32Range requires a uint32 value");
    setRange(Range::NewUInt32Range(alloc, 0, ARGS_LENGTH_MAX));
}

void
MBoundsCheck::computeRange(TempAllocator& alloc)
{
    // Just transfer the incoming index range to the output. The length() is
    // also interesting, but it is handled as a bailout check, and we're
    // computing a pre-bailout range here.
    setRange(new(alloc) Range(index()));
}

void
MSpectreMaskIndex::computeRange(TempAllocator& alloc)
{
    // Just transfer the incoming index range to the output for now.
    setRange(new(alloc) Range(index()));
}

void
MArrayPush::computeRange(TempAllocator& alloc)
{
    // MArrayPush returns the new array length.
    setRange(Range::NewUInt32Range(alloc, 0, UINT32_MAX));
}

void
MMathFunction::computeRange(TempAllocator& alloc)
{
    Range opRange(getOperand(0));
    switch (function()) {
      case Sin:
      case Cos:
        if (!opRange.canBeInfiniteOrNaN())
            setRange(Range::NewDoubleRange(alloc, -1.0, 1.0));
        break;
      case Sign:
        setRange(Range::sign(alloc, &opRange));
        break;
    default:
        break;
    }
}

void
MRandom::computeRange(TempAllocator& alloc)
{
    Range* r = Range::NewDoubleRange(alloc, 0.0, 1.0);

    // Random never returns negative zero.
    r->refineToExcludeNegativeZero();

    setRange(r);
}

void
MNaNToZero::computeRange(TempAllocator& alloc)
{
    Range other(input());
    setRange(Range::NaNToZero(alloc, &other));
}

///////////////////////////////////////////////////////////////////////////////
// Range Analysis
///////////////////////////////////////////////////////////////////////////////

bool
RangeAnalysis::analyzeLoop(MBasicBlock* header)
{
    MOZ_ASSERT(header->hasUniqueBackedge());

    // Try to compute an upper bound on the number of times the loop backedge
    // will be taken. Look for tests that dominate the backedge and which have
    // an edge leaving the loop body.
    MBasicBlock* backedge = header->backedge();

    // Ignore trivial infinite loops.
    if (backedge == header)
        return true;

    bool canOsr;
    size_t numBlocks = MarkLoopBlocks(graph_, header, &canOsr);

    // Ignore broken loops.
    if (numBlocks == 0)
        return true;

    LoopIterationBound* iterationBound = nullptr;

    MBasicBlock* block = backedge;
    do {
        BranchDirection direction;
        MTest* branch = block->immediateDominatorBranch(&direction);

        if (block == block->immediateDominator())
            break;

        block = block->immediateDominator();

        if (branch) {
            direction = NegateBranchDirection(direction);
            MBasicBlock* otherBlock = branch->branchSuccessor(direction);
            if (!otherBlock->isMarked()) {
                if (!alloc().ensureBallast())
                    return false;
                iterationBound =
                    analyzeLoopIterationCount(header, branch, direction);
                if (iterationBound)
                    break;
            }
        }
    } while (block != header);

    if (!iterationBound) {
        UnmarkLoopBlocks(graph_, header);
        return true;
    }

    if (!loopIterationBounds.append(iterationBound))
        return false;

#ifdef DEBUG
    if (JitSpewEnabled(JitSpew_Range)) {
        Sprinter sp(GetJitContext()->cx);
        if (!sp.init())
            return false;
        iterationBound->boundSum.dump(sp);
        JitSpew(JitSpew_Range, "computed symbolic bound on backedges: %s",
                sp.string());
    }
#endif

    // Try to compute symbolic bounds for the phi nodes at the head of this
    // loop, expressed in terms of the iteration bound just computed.

    for (MPhiIterator iter(header->phisBegin()); iter != header->phisEnd(); iter++)
        analyzeLoopPhi(iterationBound, *iter);

    if (!mir->compilingWasm()) {
        // Try to hoist any bounds checks from the loop using symbolic bounds.

        Vector<MBoundsCheck*, 0, JitAllocPolicy> hoistedChecks(alloc());

        for (ReversePostorderIterator iter(graph_.rpoBegin(header)); iter != graph_.rpoEnd(); iter++) {
            MBasicBlock* block = *iter;
            if (!block->isMarked())
                continue;

            for (MDefinitionIterator iter(block); iter; iter++) {
                MDefinition* def = *iter;
                if (def->isBoundsCheck() && def->isMovable()) {
                    if (!alloc().ensureBallast())
                        return false;
                    if (tryHoistBoundsCheck(header, def->toBoundsCheck())) {
                        if (!hoistedChecks.append(def->toBoundsCheck()))
                            return false;
                    }
                }
            }
        }

        // Note: replace all uses of the original bounds check with the
        // actual index. This is usually done during bounds check elimination,
        // but in this case it's safe to do it here since the load/store is
        // definitely not loop-invariant, so we will never move it before
        // one of the bounds checks we just added.
        for (size_t i = 0; i < hoistedChecks.length(); i++) {
            MBoundsCheck* ins = hoistedChecks[i];
            ins->replaceAllUsesWith(ins->index());
            ins->block()->discard(ins);
        }
    }

    UnmarkLoopBlocks(graph_, header);
    return true;
}

// Unbox beta nodes in order to hoist instruction properly, and not be limited
// by the beta nodes which are added after each branch.
static inline MDefinition*
DefinitionOrBetaInputDefinition(MDefinition* ins)
{
    while (ins->isBeta())
        ins = ins->toBeta()->input();
    return ins;
}

LoopIterationBound*
RangeAnalysis::analyzeLoopIterationCount(MBasicBlock* header,
                                         MTest* test, BranchDirection direction)
{
    SimpleLinearSum lhs(nullptr, 0);
    MDefinition* rhs;
    bool lessEqual;
    if (!ExtractLinearInequality(test, direction, &lhs, &rhs, &lessEqual))
        return nullptr;

    // Ensure the rhs is a loop invariant term.
    if (rhs && rhs->block()->isMarked()) {
        if (lhs.term && lhs.term->block()->isMarked())
            return nullptr;
        MDefinition* temp = lhs.term;
        lhs.term = rhs;
        rhs = temp;
        if (!SafeSub(0, lhs.constant, &lhs.constant))
            return nullptr;
        lessEqual = !lessEqual;
    }

    MOZ_ASSERT_IF(rhs, !rhs->block()->isMarked());

    // Ensure the lhs is a phi node from the start of the loop body.
    if (!lhs.term || !lhs.term->isPhi() || lhs.term->block() != header)
        return nullptr;

    // Check that the value of the lhs changes by a constant amount with each
    // loop iteration. This requires that the lhs be written in every loop
    // iteration with a value that is a constant difference from its value at
    // the start of the iteration.

    if (lhs.term->toPhi()->numOperands() != 2)
        return nullptr;

    // The first operand of the phi should be the lhs' value at the start of
    // the first executed iteration, and not a value written which could
    // replace the second operand below during the middle of execution.
    MDefinition* lhsInitial = lhs.term->toPhi()->getLoopPredecessorOperand();
    if (lhsInitial->block()->isMarked())
        return nullptr;

    // The second operand of the phi should be a value written by an add/sub
    // in every loop iteration, i.e. in a block which dominates the backedge.
    MDefinition* lhsWrite =
        DefinitionOrBetaInputDefinition(lhs.term->toPhi()->getLoopBackedgeOperand());
    if (!lhsWrite->isAdd() && !lhsWrite->isSub())
        return nullptr;
    if (!lhsWrite->block()->isMarked())
        return nullptr;
    MBasicBlock* bb = header->backedge();
    for (; bb != lhsWrite->block() && bb != header; bb = bb->immediateDominator()) {}
    if (bb != lhsWrite->block())
        return nullptr;

    SimpleLinearSum lhsModified = ExtractLinearSum(lhsWrite);

    // Check that the value of the lhs at the backedge is of the form
    // 'old(lhs) + N'. We can be sure that old(lhs) is the value at the start
    // of the iteration, and not that written to lhs in a previous iteration,
    // as such a previous value could not appear directly in the addition:
    // it could not be stored in lhs as the lhs add/sub executes in every
    // iteration, and if it were stored in another variable its use here would
    // be as an operand to a phi node for that variable.
    if (lhsModified.term != lhs.term)
        return nullptr;

    LinearSum iterationBound(alloc());
    LinearSum currentIteration(alloc());

    if (lhsModified.constant == 1 && !lessEqual) {
        // The value of lhs is 'initial(lhs) + iterCount' and this will end
        // execution of the loop if 'lhs + lhsN >= rhs'. Thus, an upper bound
        // on the number of backedges executed is:
        //
        // initial(lhs) + iterCount + lhsN == rhs
        // iterCount == rhsN - initial(lhs) - lhsN

        if (rhs) {
            if (!iterationBound.add(rhs, 1))
                return nullptr;
        }
        if (!iterationBound.add(lhsInitial, -1))
            return nullptr;

        int32_t lhsConstant;
        if (!SafeSub(0, lhs.constant, &lhsConstant))
            return nullptr;
        if (!iterationBound.add(lhsConstant))
            return nullptr;

        if (!currentIteration.add(lhs.term, 1))
            return nullptr;
        if (!currentIteration.add(lhsInitial, -1))
            return nullptr;
    } else if (lhsModified.constant == -1 && lessEqual) {
        // The value of lhs is 'initial(lhs) - iterCount'. Similar to the above
        // case, an upper bound on the number of backedges executed is:
        //
        // initial(lhs) - iterCount + lhsN == rhs
        // iterCount == initial(lhs) - rhs + lhsN

        if (!iterationBound.add(lhsInitial, 1))
            return nullptr;
        if (rhs) {
            if (!iterationBound.add(rhs, -1))
                return nullptr;
        }
        if (!iterationBound.add(lhs.constant))
            return nullptr;

        if (!currentIteration.add(lhsInitial, 1))
            return nullptr;
        if (!currentIteration.add(lhs.term, -1))
            return nullptr;
    } else {
        return nullptr;
    }

    return new(alloc()) LoopIterationBound(header, test, iterationBound, currentIteration);
}

void
RangeAnalysis::analyzeLoopPhi(LoopIterationBound* loopBound, MPhi* phi)
{
    // Given a bound on the number of backedges taken, compute an upper and
    // lower bound for a phi node that may change by a constant amount each
    // iteration. Unlike for the case when computing the iteration bound
    // itself, the phi does not need to change the same amount every iteration,
    // but is required to change at most N and be either nondecreasing or
    // nonincreasing.

    MOZ_ASSERT(phi->numOperands() == 2);

    MDefinition* initial = phi->getLoopPredecessorOperand();
    if (initial->block()->isMarked())
        return;

    SimpleLinearSum modified = ExtractLinearSum(phi->getLoopBackedgeOperand());

    if (modified.term != phi || modified.constant == 0)
        return;

    if (!phi->range())
        phi->setRange(new(alloc()) Range(phi));

    LinearSum initialSum(alloc());
    if (!initialSum.add(initial, 1))
        return;

    // The phi may change by N each iteration, and is either nondecreasing or
    // nonincreasing. initial(phi) is either a lower or upper bound for the
    // phi, and initial(phi) + loopBound * N is either an upper or lower bound,
    // at all points within the loop, provided that loopBound >= 0.
    //
    // We are more interested, however, in the bound for phi at points
    // dominated by the loop bound's test; if the test dominates e.g. a bounds
    // check we want to hoist from the loop, using the value of the phi at the
    // head of the loop for this will usually be too imprecise to hoist the
    // check. These points will execute only if the backedge executes at least
    // one more time (as the test passed and the test dominates the backedge),
    // so we know both that loopBound >= 1 and that the phi's value has changed
    // at most loopBound - 1 times. Thus, another upper or lower bound for the
    // phi is initial(phi) + (loopBound - 1) * N, without requiring us to
    // ensure that loopBound >= 0.

    LinearSum limitSum(loopBound->boundSum);
    if (!limitSum.multiply(modified.constant) || !limitSum.add(initialSum))
        return;

    int32_t negativeConstant;
    if (!SafeSub(0, modified.constant, &negativeConstant) || !limitSum.add(negativeConstant))
        return;

    Range* initRange = initial->range();
    if (modified.constant > 0) {
        if (initRange && initRange->hasInt32LowerBound())
            phi->range()->refineLower(initRange->lower());
        phi->range()->setSymbolicLower(SymbolicBound::New(alloc(), nullptr, initialSum));
        phi->range()->setSymbolicUpper(SymbolicBound::New(alloc(), loopBound, limitSum));
    } else {
        if (initRange && initRange->hasInt32UpperBound())
            phi->range()->refineUpper(initRange->upper());
        phi->range()->setSymbolicUpper(SymbolicBound::New(alloc(), nullptr, initialSum));
        phi->range()->setSymbolicLower(SymbolicBound::New(alloc(), loopBound, limitSum));
    }

    JitSpew(JitSpew_Range, "added symbolic range on %d", phi->id());
    SpewRange(phi);
}

// Whether bound is valid at the specified bounds check instruction in a loop,
// and may be used to hoist ins.
static inline bool
SymbolicBoundIsValid(MBasicBlock* header, MBoundsCheck* ins, const SymbolicBound* bound)
{
    if (!bound->loop)
        return true;
    if (ins->block() == header)
        return false;
    MBasicBlock* bb = ins->block()->immediateDominator();
    while (bb != header && bb != bound->loop->test->block())
        bb = bb->immediateDominator();
    return bb == bound->loop->test->block();
}

bool
RangeAnalysis::tryHoistBoundsCheck(MBasicBlock* header, MBoundsCheck* ins)
{
    // The bounds check's length must be loop invariant.
    MDefinition *length = DefinitionOrBetaInputDefinition(ins->length());
    if (length->block()->isMarked())
        return false;

    // The bounds check's index should not be loop invariant (else we would
    // already have hoisted it during LICM).
    SimpleLinearSum index = ExtractLinearSum(ins->index());
    if (!index.term || !index.term->block()->isMarked())
        return false;

    // Check for a symbolic lower and upper bound on the index. If either
    // condition depends on an iteration bound for the loop, only hoist if
    // the bounds check is dominated by the iteration bound's test.
    if (!index.term->range())
        return false;
    const SymbolicBound* lower = index.term->range()->symbolicLower();
    if (!lower || !SymbolicBoundIsValid(header, ins, lower))
        return false;
    const SymbolicBound* upper = index.term->range()->symbolicUpper();
    if (!upper || !SymbolicBoundIsValid(header, ins, upper))
        return false;

    MBasicBlock* preLoop = header->loopPredecessor();
    MOZ_ASSERT(!preLoop->isMarked());

    MDefinition* lowerTerm = ConvertLinearSum(alloc(), preLoop, lower->sum);
    if (!lowerTerm)
        return false;

    MDefinition* upperTerm = ConvertLinearSum(alloc(), preLoop, upper->sum);
    if (!upperTerm)
        return false;

    // We are checking that index + indexConstant >= 0, and know that
    // index >= lowerTerm + lowerConstant. Thus, check that:
    //
    // lowerTerm + lowerConstant + indexConstant >= 0
    // lowerTerm >= -lowerConstant - indexConstant

    int32_t lowerConstant = 0;
    if (!SafeSub(lowerConstant, index.constant, &lowerConstant))
        return false;
    if (!SafeSub(lowerConstant, lower->sum.constant(), &lowerConstant))
        return false;

    // We are checking that index < boundsLength, and know that
    // index <= upperTerm + upperConstant. Thus, check that:
    //
    // upperTerm + upperConstant < boundsLength

    int32_t upperConstant = index.constant;
    if (!SafeAdd(upper->sum.constant(), upperConstant, &upperConstant))
        return false;

    // Hoist the loop invariant lower bounds checks.
    MBoundsCheckLower* lowerCheck = MBoundsCheckLower::New(alloc(), lowerTerm);
    lowerCheck->setMinimum(lowerConstant);
    lowerCheck->computeRange(alloc());
    lowerCheck->collectRangeInfoPreTrunc();
    preLoop->insertBefore(preLoop->lastIns(), lowerCheck);

    // Hoist the loop invariant upper bounds checks.
    if (upperTerm != length || upperConstant >= 0) {
        MBoundsCheck* upperCheck = MBoundsCheck::New(alloc(), upperTerm, length);
        upperCheck->setMinimum(upperConstant);
        upperCheck->setMaximum(upperConstant);
        upperCheck->computeRange(alloc());
        upperCheck->collectRangeInfoPreTrunc();
        preLoop->insertBefore(preLoop->lastIns(), upperCheck);
    }

    return true;
}

bool
RangeAnalysis::analyze()
{
    JitSpew(JitSpew_Range, "Doing range propagation");

    for (ReversePostorderIterator iter(graph_.rpoBegin()); iter != graph_.rpoEnd(); iter++) {
        MBasicBlock* block = *iter;
        // No blocks are supposed to be unreachable, except when we have an OSR
        // block, in which case the Value Numbering phase add fixup blocks which
        // are unreachable.
        MOZ_ASSERT(!block->unreachable() || graph_.osrBlock());

        // If the block's immediate dominator is unreachable, the block is
        // unreachable. Iterating in RPO, we'll always see the immediate
        // dominator before the block.
        if (block->immediateDominator()->unreachable()) {
            block->setUnreachableUnchecked();
            continue;
        }

        for (MDefinitionIterator iter(block); iter; iter++) {
            MDefinition* def = *iter;
            if (!alloc().ensureBallast())
                return false;

            def->computeRange(alloc());
            JitSpew(JitSpew_Range, "computing range on %d", def->id());
            SpewRange(def);
        }

        // Beta node range analysis may have marked this block unreachable. If
        // so, it's no longer interesting to continue processing it.
        if (block->unreachable())
            continue;

        if (block->isLoopHeader()) {
            if (!analyzeLoop(block))
                return false;
        }

        // First pass at collecting range info - while the beta nodes are still
        // around and before truncation.
        for (MInstructionIterator iter(block->begin()); iter != block->end(); iter++)
            iter->collectRangeInfoPreTrunc();
    }

    return true;
}

bool
RangeAnalysis::addRangeAssertions()
{
    if (!JitOptions.checkRangeAnalysis)
        return true;

    // Check the computed range for this instruction, if the option is set. Note
    // that this code is quite invasive; it adds numerous additional
    // instructions for each MInstruction with a computed range, and it uses
    // registers, so it also affects register allocation.
    for (ReversePostorderIterator iter(graph_.rpoBegin()); iter != graph_.rpoEnd(); iter++) {
        MBasicBlock* block = *iter;

        // Do not add assertions in unreachable blocks.
        if (block->unreachable())
            continue;

        for (MDefinitionIterator iter(block); iter; iter++) {
            MDefinition* ins = *iter;

            // Perform range checking for all numeric and numeric-like types.
            if (!IsNumberType(ins->type()) &&
                ins->type() != MIRType::Boolean &&
                ins->type() != MIRType::Value)
            {
                continue;
            }

            // MIsNoIter is fused with the MTest that follows it and emitted as
            // LIsNoIterAndBranch. Skip it to avoid complicating MIsNoIter
            // lowering.
            if (ins->isIsNoIter())
                continue;

            Range r(ins);

            MOZ_ASSERT_IF(ins->type() == MIRType::Int64, r.isUnknown());

            // Don't insert assertions if there's nothing interesting to assert.
            if (r.isUnknown() || (ins->type() == MIRType::Int32 && r.isUnknownInt32()))
                continue;

            // Don't add a use to an instruction that is recovered on bailout.
            if (ins->isRecoveredOnBailout())
                continue;

            if (!alloc().ensureBallast())
                return false;
            MAssertRange* guard = MAssertRange::New(alloc(), ins, new(alloc()) Range(r));

            // Beta nodes and interrupt checks are required to be located at the
            // beginnings of basic blocks, so we must insert range assertions
            // after any such instructions.
            MInstruction* insertAt = nullptr;
            if (block->graph().osrBlock() == block)
                insertAt = ins->toInstruction();
            else
                insertAt = block->safeInsertTop(ins);

            if (insertAt == *iter)
                block->insertAfter(insertAt,  guard);
            else
                block->insertBefore(insertAt, guard);
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Range based Truncation
///////////////////////////////////////////////////////////////////////////////

void
Range::clampToInt32()
{
    if (isInt32())
        return;
    int32_t l = hasInt32LowerBound() ? lower() : JSVAL_INT_MIN;
    int32_t h = hasInt32UpperBound() ? upper() : JSVAL_INT_MAX;
    setInt32(l, h);
}

void
Range::wrapAroundToInt32()
{
    if (!hasInt32Bounds()) {
        setInt32(JSVAL_INT_MIN, JSVAL_INT_MAX);
    } else if (canHaveFractionalPart()) {
        // Clearing the fractional field may provide an opportunity to refine
        // lower_ or upper_.
        canHaveFractionalPart_ = ExcludesFractionalParts;
        canBeNegativeZero_ = ExcludesNegativeZero;
        refineInt32BoundsByExponent(max_exponent_,
                                    &lower_, &hasInt32LowerBound_,
                                    &upper_, &hasInt32UpperBound_);

        assertInvariants();
    } else {
        // If nothing else, we can clear the negative zero flag.
        canBeNegativeZero_ = ExcludesNegativeZero;
    }
    MOZ_ASSERT(isInt32());
}

void
Range::wrapAroundToShiftCount()
{
    wrapAroundToInt32();
    if (lower() < 0 || upper() >= 32)
        setInt32(0, 31);
}

void
Range::wrapAroundToBoolean()
{
    wrapAroundToInt32();
    if (!isBoolean())
        setInt32(0, 1);
    MOZ_ASSERT(isBoolean());
}

bool
MDefinition::needTruncation(TruncateKind kind)
{
    // No procedure defined for truncating this instruction.
    return false;
}

void
MDefinition::truncate()
{
    MOZ_CRASH("No procedure defined for truncating this instruction.");
}

bool
MConstant::needTruncation(TruncateKind kind)
{
    return IsFloatingPointType(type());
}

void
MConstant::truncate()
{
    MOZ_ASSERT(needTruncation(Truncate));

    // Truncate the double to int, since all uses truncates it.
    int32_t res = ToInt32(numberToDouble());
    payload_.asBits = 0;
    payload_.i32 = res;
    setResultType(MIRType::Int32);
    if (range())
        range()->setInt32(res, res);
}

bool
MPhi::needTruncation(TruncateKind kind)
{
    if (type() == MIRType::Double || type() == MIRType::Int32) {
        truncateKind_ = kind;
        return true;
    }

    return false;
}

void
MPhi::truncate()
{
    setResultType(MIRType::Int32);
    if (truncateKind_ >= IndirectTruncate && range())
        range()->wrapAroundToInt32();
}

bool
MAdd::needTruncation(TruncateKind kind)
{
    // Remember analysis, needed for fallible checks.
    setTruncateKind(kind);

    return type() == MIRType::Double || type() == MIRType::Int32;
}

void
MAdd::truncate()
{
    MOZ_ASSERT(needTruncation(truncateKind()));
    specialization_ = MIRType::Int32;
    setResultType(MIRType::Int32);
    if (truncateKind() >= IndirectTruncate && range())
        range()->wrapAroundToInt32();
}

bool
MSub::needTruncation(TruncateKind kind)
{
    // Remember analysis, needed for fallible checks.
    setTruncateKind(kind);

    return type() == MIRType::Double || type() == MIRType::Int32;
}

void
MSub::truncate()
{
    MOZ_ASSERT(needTruncation(truncateKind()));
    specialization_ = MIRType::Int32;
    setResultType(MIRType::Int32);
    if (truncateKind() >= IndirectTruncate && range())
        range()->wrapAroundToInt32();
}

bool
MMul::needTruncation(TruncateKind kind)
{
    // Remember analysis, needed for fallible checks.
    setTruncateKind(kind);

    return type() == MIRType::Double || type() == MIRType::Int32;
}

void
MMul::truncate()
{
    MOZ_ASSERT(needTruncation(truncateKind()));
    specialization_ = MIRType::Int32;
    setResultType(MIRType::Int32);
    if (truncateKind() >= IndirectTruncate) {
        setCanBeNegativeZero(false);
        if (range())
            range()->wrapAroundToInt32();
    }
}

bool
MDiv::needTruncation(TruncateKind kind)
{
    // Remember analysis, needed for fallible checks.
    setTruncateKind(kind);

    return type() == MIRType::Double || type() == MIRType::Int32;
}

void
MDiv::truncate()
{
    MOZ_ASSERT(needTruncation(truncateKind()));
    specialization_ = MIRType::Int32;
    setResultType(MIRType::Int32);

    // Divisions where the lhs and rhs are unsigned and the result is
    // truncated can be lowered more efficiently.
    if (unsignedOperands()) {
        replaceWithUnsignedOperands();
        unsigned_ = true;
    }
}

bool
MMod::needTruncation(TruncateKind kind)
{
    // Remember analysis, needed for fallible checks.
    setTruncateKind(kind);

    return type() == MIRType::Double || type() == MIRType::Int32;
}

void
MMod::truncate()
{
    // As for division, handle unsigned modulus with a truncated result.
    MOZ_ASSERT(needTruncation(truncateKind()));
    specialization_ = MIRType::Int32;
    setResultType(MIRType::Int32);

    if (unsignedOperands()) {
        replaceWithUnsignedOperands();
        unsigned_ = true;
    }
}

bool
MToDouble::needTruncation(TruncateKind kind)
{
    MOZ_ASSERT(type() == MIRType::Double);
    setTruncateKind(kind);

    return true;
}

void
MToDouble::truncate()
{
    MOZ_ASSERT(needTruncation(truncateKind()));

    // We use the return type to flag that this MToDouble should be replaced by
    // a MTruncateToInt32 when modifying the graph.
    setResultType(MIRType::Int32);
    if (truncateKind() >= IndirectTruncate) {
        if (range())
            range()->wrapAroundToInt32();
    }
}

bool
MLimitedTruncate::needTruncation(TruncateKind kind)
{
    setTruncateKind(kind);
    setResultType(MIRType::Int32);
    if (kind >= IndirectTruncate && range())
        range()->wrapAroundToInt32();
    return false;
}

bool
MCompare::needTruncation(TruncateKind kind)
{
    // If we're compiling wasm, don't try to optimize the comparison type, as
    // the code presumably is already using the type it wants. Also, wasm
    // doesn't support bailouts, so we woudn't be able to rely on
    // TruncateAfterBailouts to convert our inputs.
    if (block()->info().compilingWasm())
       return false;

    if (!isDoubleComparison())
        return false;

    // If both operands are naturally in the int32 range, we can convert from
    // a double comparison to being an int32 comparison.
    if (!Range(lhs()).isInt32() || !Range(rhs()).isInt32())
        return false;

    return true;
}

void
MCompare::truncate()
{
    compareType_ = Compare_Int32;

    // Truncating the operands won't change their value because we don't force a
    // truncation, but it will change their type, which we need because we
    // now expect integer inputs.
    truncateOperands_ = true;
}

MDefinition::TruncateKind
MDefinition::operandTruncateKind(size_t index) const
{
    // Generic routine: We don't know anything.
    return NoTruncate;
}

MDefinition::TruncateKind
MPhi::operandTruncateKind(size_t index) const
{
    // The truncation applied to a phi is effectively applied to the phi's
    // operands.
    return truncateKind_;
}

MDefinition::TruncateKind
MTruncateToInt32::operandTruncateKind(size_t index) const
{
    // This operator is an explicit truncate to int32.
    return Truncate;
}

MDefinition::TruncateKind
MBinaryBitwiseInstruction::operandTruncateKind(size_t index) const
{
    // The bitwise operators truncate to int32.
    return Truncate;
}

MDefinition::TruncateKind
MLimitedTruncate::operandTruncateKind(size_t index) const
{
    return Min(truncateKind(), truncateLimit_);
}

MDefinition::TruncateKind
MAdd::operandTruncateKind(size_t index) const
{
    // This operator is doing some arithmetic. If its result is truncated,
    // it's an indirect truncate for its operands.
    return Min(truncateKind(), IndirectTruncate);
}

MDefinition::TruncateKind
MSub::operandTruncateKind(size_t index) const
{
    // See the comment in MAdd::operandTruncateKind.
    return Min(truncateKind(), IndirectTruncate);
}

MDefinition::TruncateKind
MMul::operandTruncateKind(size_t index) const
{
    // See the comment in MAdd::operandTruncateKind.
    return Min(truncateKind(), IndirectTruncate);
}

MDefinition::TruncateKind
MToDouble::operandTruncateKind(size_t index) const
{
    // MToDouble propagates its truncate kind to its operand.
    return truncateKind();
}

MDefinition::TruncateKind
MStoreUnboxedScalar::operandTruncateKind(size_t index) const
{
    // Some receiver objects, such as typed arrays, will truncate out of range integer inputs.
    return (truncateInput() && index == 2 && isIntegerWrite()) ? Truncate : NoTruncate;
}

MDefinition::TruncateKind
MStoreTypedArrayElementHole::operandTruncateKind(size_t index) const
{
    // An integer store truncates the stored value.
    return index == 3 && isIntegerWrite() ? Truncate : NoTruncate;
}

MDefinition::TruncateKind
MDiv::operandTruncateKind(size_t index) const
{
    return Min(truncateKind(), TruncateAfterBailouts);
}

MDefinition::TruncateKind
MMod::operandTruncateKind(size_t index) const
{
    return Min(truncateKind(), TruncateAfterBailouts);
}

MDefinition::TruncateKind
MCompare::operandTruncateKind(size_t index) const
{
    // If we're doing an int32 comparison on operands which were previously
    // floating-point, convert them!
    MOZ_ASSERT_IF(truncateOperands_, isInt32Comparison());
    return truncateOperands_ ? TruncateAfterBailouts : NoTruncate;
}

static bool
TruncateTest(TempAllocator& alloc, MTest* test)
{
    // If all possible inputs to the test are either int32 or boolean,
    // convert those inputs to int32 so that an int32 test can be performed.

    if (test->input()->type() != MIRType::Value)
        return true;

    if (!test->input()->isPhi() || !test->input()->hasOneDefUse() || test->input()->isImplicitlyUsed())
        return true;

    MPhi* phi = test->input()->toPhi();
    for (size_t i = 0; i < phi->numOperands(); i++) {
        MDefinition* def = phi->getOperand(i);
        if (!def->isBox())
            return true;
        MDefinition* inner = def->getOperand(0);
        if (inner->type() != MIRType::Boolean && inner->type() != MIRType::Int32)
            return true;
    }

    for (size_t i = 0; i < phi->numOperands(); i++) {
        MDefinition* inner = phi->getOperand(i)->getOperand(0);
        if (inner->type() != MIRType::Int32) {
            if (!alloc.ensureBallast())
                return false;
            MBasicBlock* block = inner->block();
            inner = MToNumberInt32::New(alloc, inner);
            block->insertBefore(block->lastIns(), inner->toInstruction());
        }
        MOZ_ASSERT(inner->type() == MIRType::Int32);
        phi->replaceOperand(i, inner);
    }

    phi->setResultType(MIRType::Int32);
    return true;
}

// Truncating instruction result is an optimization which implies
// knowing all uses of an instruction.  This implies that if one of
// the uses got removed, then Range Analysis is not be allowed to do
// any modification which can change the result, especially if the
// result can be observed.
//
// This corner can easily be understood with UCE examples, but it
// might also happen with type inference assumptions.  Note: Type
// inference is implicitly branches where other types might be
// flowing into.
static bool
CloneForDeadBranches(TempAllocator& alloc, MInstruction* candidate)
{
    // Compare returns a boolean so it doesn't have to be recovered on bailout
    // because the output would remain correct.
    if (candidate->isCompare())
        return true;

    MOZ_ASSERT(candidate->canClone());
    if (!alloc.ensureBallast())
        return false;

    MDefinitionVector operands(alloc);
    size_t end = candidate->numOperands();
    if (!operands.reserve(end))
        return false;
    for (size_t i = 0; i < end; ++i)
        operands.infallibleAppend(candidate->getOperand(i));

    MInstruction* clone = candidate->clone(alloc, operands);
    clone->setRange(nullptr);

    // Set UseRemoved flag on the cloned instruction in order to chain recover
    // instruction for the bailout path.
    clone->setUseRemovedUnchecked();

    candidate->block()->insertBefore(candidate, clone);

    if (!candidate->maybeConstantValue()) {
        MOZ_ASSERT(clone->canRecoverOnBailout());
        clone->setRecoveredOnBailout();
    }

    // Replace the candidate by its recovered on bailout clone within recovered
    // instructions and resume points operands.
    for (MUseIterator i(candidate->usesBegin()); i != candidate->usesEnd(); ) {
        MUse* use = *i++;
        MNode* ins = use->consumer();
        if (ins->isDefinition() && !ins->toDefinition()->isRecoveredOnBailout())
            continue;

        use->replaceProducer(clone);
    }

    return true;
}

// Examine all the users of |candidate| and determine the most aggressive
// truncate kind that satisfies all of them.
static MDefinition::TruncateKind
ComputeRequestedTruncateKind(MDefinition* candidate, bool* shouldClone)
{
    bool isCapturedResult = false;   // Check if used by a recovered instruction or a resume point.
    bool isObservableResult = false; // Check if it can be read from another frame.
    bool isRecoverableResult = true; // Check if it can safely be reconstructed.
    bool hasUseRemoved = candidate->isUseRemoved();

    MDefinition::TruncateKind kind = MDefinition::Truncate;
    for (MUseIterator use(candidate->usesBegin()); use != candidate->usesEnd(); use++) {
        if (use->consumer()->isResumePoint()) {
            // Truncation is a destructive optimization, as such, we need to pay
            // attention to removed branches and prevent optimization
            // destructive optimizations if we have no alternative. (see
            // UseRemoved flag)
            isCapturedResult = true;
            isObservableResult = isObservableResult ||
                use->consumer()->toResumePoint()->isObservableOperand(*use);
            isRecoverableResult = isRecoverableResult &&
                use->consumer()->toResumePoint()->isRecoverableOperand(*use);
            continue;
        }

        MDefinition* consumer = use->consumer()->toDefinition();
        if (consumer->isRecoveredOnBailout()) {
            isCapturedResult = true;
            hasUseRemoved = hasUseRemoved || consumer->isUseRemoved();
            continue;
        }

        MDefinition::TruncateKind consumerKind = consumer->operandTruncateKind(consumer->indexOf(*use));
        kind = Min(kind, consumerKind);
        if (kind == MDefinition::NoTruncate)
            break;
    }

    // We cannot do full trunction on guarded instructions.
    if (candidate->isGuard() || candidate->isGuardRangeBailouts())
        kind = Min(kind, MDefinition::TruncateAfterBailouts);

    // If the value naturally produces an int32 value (before bailout checks)
    // that needs no conversion, we don't have to worry about resume points
    // seeing truncated values.
    bool needsConversion = !candidate->range() || !candidate->range()->isInt32();

    // If the instruction is explicitly truncated (not indirectly) by all its
    // uses and if it has no removed uses, then we can safely encode its
    // truncated result as part of the resume point operands.  This is safe,
    // because even if we resume with a truncated double, the next baseline
    // instruction operating on this instruction is going to be a no-op.
    //
    // Note, that if the result can be observed from another frame, then this
    // optimization is not safe.
    bool safeToConvert = kind == MDefinition::Truncate && !hasUseRemoved && !isObservableResult;

    // If the candidate instruction appears as operand of a resume point or a
    // recover instruction, and we have to truncate its result, then we might
    // have to either recover the result during the bailout, or avoid the
    // truncation.
    if (isCapturedResult && needsConversion && !safeToConvert) {

        // If the result can be recovered from all the resume points (not needed
        // for iterating over the inlined frames), and this instruction can be
        // recovered on bailout, then we can clone it and use the cloned
        // instruction to encode the recover instruction.  Otherwise, we should
        // keep the original result and bailout if the value is not in the int32
        // range.
        if (!JitOptions.disableRecoverIns && isRecoverableResult && candidate->canRecoverOnBailout())
            *shouldClone = true;
        else
            kind = Min(kind, MDefinition::TruncateAfterBailouts);
    }

    return kind;
}

static MDefinition::TruncateKind
ComputeTruncateKind(MDefinition* candidate, bool* shouldClone)
{
    // Compare operations might coerce its inputs to int32 if the ranges are
    // correct.  So we do not need to check if all uses are coerced.
    if (candidate->isCompare())
        return MDefinition::TruncateAfterBailouts;

    // Set truncated flag if range analysis ensure that it has no
    // rounding errors and no fractional part. Note that we can't use
    // the MDefinition Range constructor, because we need to know if
    // the value will have rounding errors before any bailout checks.
    const Range* r = candidate->range();
    bool canHaveRoundingErrors = !r || r->canHaveRoundingErrors();

    // Special case integer division and modulo: a/b can be infinite, and a%b
    // can be NaN but cannot actually have rounding errors induced by truncation.
    if ((candidate->isDiv() || candidate->isMod()) &&
        static_cast<const MBinaryArithInstruction *>(candidate)->specialization() == MIRType::Int32)
    {
        canHaveRoundingErrors = false;
    }

    if (canHaveRoundingErrors)
        return MDefinition::NoTruncate;

    // Ensure all observable uses are truncated.
    return ComputeRequestedTruncateKind(candidate, shouldClone);
}

static void
RemoveTruncatesOnOutput(MDefinition* truncated)
{
    // Compare returns a boolean so it doen't have any output truncates.
    if (truncated->isCompare())
        return;

    MOZ_ASSERT(truncated->type() == MIRType::Int32);
    MOZ_ASSERT(Range(truncated).isInt32());

    for (MUseDefIterator use(truncated); use; use++) {
        MDefinition* def = use.def();
        if (!def->isTruncateToInt32() || !def->isToNumberInt32())
            continue;

        def->replaceAllUsesWith(truncated);
    }
}

static void
AdjustTruncatedInputs(TempAllocator& alloc, MDefinition* truncated)
{
    MBasicBlock* block = truncated->block();
    for (size_t i = 0, e = truncated->numOperands(); i < e; i++) {
        MDefinition::TruncateKind kind = truncated->operandTruncateKind(i);
        if (kind == MDefinition::NoTruncate)
            continue;

        MDefinition* input = truncated->getOperand(i);
        if (input->type() == MIRType::Int32)
            continue;

        if (input->isToDouble() && input->getOperand(0)->type() == MIRType::Int32) {
            truncated->replaceOperand(i, input->getOperand(0));
        } else {
            MInstruction* op;
            if (kind == MDefinition::TruncateAfterBailouts)
                op = MToNumberInt32::New(alloc, truncated->getOperand(i));
            else
                op = MTruncateToInt32::New(alloc, truncated->getOperand(i));

            if (truncated->isPhi()) {
                MBasicBlock* pred = block->getPredecessor(i);
                pred->insertBefore(pred->lastIns(), op);
            } else {
                block->insertBefore(truncated->toInstruction(), op);
            }
            truncated->replaceOperand(i, op);
        }
    }

    if (truncated->isToDouble()) {
        truncated->replaceAllUsesWith(truncated->toToDouble()->getOperand(0));
        block->discard(truncated->toToDouble());
    }
}

// Iterate backward on all instruction and attempt to truncate operations for
// each instruction which respect the following list of predicates: Has been
// analyzed by range analysis, the range has no rounding errors, all uses cases
// are truncating the result.
//
// If the truncation of the operation is successful, then the instruction is
// queue for later updating the graph to restore the type correctness by
// converting the operands that need to be truncated.
//
// We iterate backward because it is likely that a truncated operation truncates
// some of its operands.
bool
RangeAnalysis::truncate()
{
    JitSpew(JitSpew_Range, "Do range-base truncation (backward loop)");

    // Automatic truncation is disabled for wasm because the truncation logic
    // is based on IonMonkey which assumes that we can bailout if the truncation
    // logic fails. As wasm code has no bailout mechanism, it is safer to avoid
    // any automatic truncations.
    MOZ_ASSERT(!mir->compilingWasm());

    Vector<MDefinition*, 16, SystemAllocPolicy> worklist;

    for (PostorderIterator block(graph_.poBegin()); block != graph_.poEnd(); block++) {
        for (MInstructionReverseIterator iter(block->rbegin()); iter != block->rend(); iter++) {
            if (iter->isRecoveredOnBailout())
                continue;

            if (iter->type() == MIRType::None) {
                if (iter->isTest()) {
                    if (!TruncateTest(alloc(), iter->toTest()))
                        return false;
                }
                continue;
            }

            // Remember all bitop instructions for folding after range analysis.
            switch (iter->op()) {
              case MDefinition::Opcode::BitAnd:
              case MDefinition::Opcode::BitOr:
              case MDefinition::Opcode::BitXor:
              case MDefinition::Opcode::Lsh:
              case MDefinition::Opcode::Rsh:
              case MDefinition::Opcode::Ursh:
                if (!bitops.append(static_cast<MBinaryBitwiseInstruction*>(*iter)))
                    return false;
                break;
              default:;
            }

            bool shouldClone = false;
            MDefinition::TruncateKind kind = ComputeTruncateKind(*iter, &shouldClone);
            if (kind == MDefinition::NoTruncate)
                continue;

            // Range Analysis is sometimes eager to do optimizations, even if we
            // are not be able to truncate an instruction. In such case, we
            // speculatively compile the instruction to an int32 instruction
            // while adding a guard. This is what is implied by
            // TruncateAfterBailout.
            //
            // If we already experienced an overflow bailout while executing
            // code within the current JSScript, we no longer attempt to make
            // this kind of eager optimizations.
            if (kind <= MDefinition::TruncateAfterBailouts && block->info().hadOverflowBailout())
                continue;

            // Truncate this instruction if possible.
            if (!iter->needTruncation(kind))
                continue;

            SpewTruncate(*iter, kind, shouldClone);

            // If needed, clone the current instruction for keeping it for the
            // bailout path.  This give us the ability to truncate instructions
            // even after the removal of branches.
            if (shouldClone && !CloneForDeadBranches(alloc(), *iter))
                return false;

            iter->truncate();

            // Delay updates of inputs/outputs to avoid creating node which
            // would be removed by the truncation of the next operations.
            iter->setInWorklist();
            if (!worklist.append(*iter))
                return false;
        }
        for (MPhiIterator iter(block->phisBegin()), end(block->phisEnd()); iter != end; ++iter) {
            bool shouldClone = false;
            MDefinition::TruncateKind kind = ComputeTruncateKind(*iter, &shouldClone);
            if (kind == MDefinition::NoTruncate)
                continue;

            // Truncate this phi if possible.
            if (shouldClone || !iter->needTruncation(kind))
                continue;

            SpewTruncate(*iter, kind, shouldClone);

            iter->truncate();

            // Delay updates of inputs/outputs to avoid creating node which
            // would be removed by the truncation of the next operations.
            iter->setInWorklist();
            if (!worklist.append(*iter))
                return false;
        }
    }

    // Update inputs/outputs of truncated instructions.
    JitSpew(JitSpew_Range, "Do graph type fixup (dequeue)");
    while (!worklist.empty()) {
        if (!alloc().ensureBallast())
            return false;
        MDefinition* def = worklist.popCopy();
        def->setNotInWorklist();
        RemoveTruncatesOnOutput(def);
        AdjustTruncatedInputs(alloc(), def);
    }

    return true;
}

bool
RangeAnalysis::removeUnnecessaryBitops()
{
    // Note: This operation change the semantic of the program in a way which
    // uniquely works with Int32, Recover Instructions added by the Sink phase
    // expects the MIR Graph to still have a valid flow as-if they were double
    // operations instead of Int32 operations. Thus, this phase should be
    // executed after the Sink phase, and before DCE.

    // Fold any unnecessary bitops in the graph, such as (x | 0) on an integer
    // input. This is done after range analysis rather than during GVN as the
    // presence of the bitop can change which instructions are truncated.
    for (size_t i = 0; i < bitops.length(); i++) {
        MBinaryBitwiseInstruction* ins = bitops[i];
        if (ins->isRecoveredOnBailout())
            continue;

        MDefinition* folded = ins->foldUnnecessaryBitop();
        if (folded != ins) {
            ins->replaceAllLiveUsesWith(folded);
            ins->setRecoveredOnBailout();
        }
    }

    bitops.clear();
    return true;
}


///////////////////////////////////////////////////////////////////////////////
// Collect Range information of operands
///////////////////////////////////////////////////////////////////////////////

void
MInArray::collectRangeInfoPreTrunc()
{
    Range indexRange(index());
    if (indexRange.isFiniteNonNegative())
        needsNegativeIntCheck_ = false;
}

void
MLoadElementHole::collectRangeInfoPreTrunc()
{
    Range indexRange(index());
    if (indexRange.isFiniteNonNegative()) {
        needsNegativeIntCheck_ = false;
        setNotGuard();
    }
}

void
MClz::collectRangeInfoPreTrunc()
{
    Range inputRange(input());
    if (!inputRange.canBeZero())
        operandIsNeverZero_ = true;
}

void
MCtz::collectRangeInfoPreTrunc()
{
    Range inputRange(input());
    if (!inputRange.canBeZero())
        operandIsNeverZero_ = true;
}

void
MDiv::collectRangeInfoPreTrunc()
{
    Range lhsRange(lhs());
    Range rhsRange(rhs());

    // Test if Dividend is non-negative.
    if (lhsRange.isFiniteNonNegative())
        canBeNegativeDividend_ = false;

    // Try removing divide by zero check.
    if (!rhsRange.canBeZero())
        canBeDivideByZero_ = false;

    // If lhsRange does not contain INT32_MIN in its range,
    // negative overflow check can be skipped.
    if (!lhsRange.contains(INT32_MIN))
        canBeNegativeOverflow_ = false;

    // If rhsRange does not contain -1 likewise.
    if (!rhsRange.contains(-1))
        canBeNegativeOverflow_ = false;

    // If lhsRange does not contain a zero,
    // negative zero check can be skipped.
    if (!lhsRange.canBeZero())
        canBeNegativeZero_ = false;

    // If rhsRange >= 0 negative zero check can be skipped.
    if (rhsRange.isFiniteNonNegative())
        canBeNegativeZero_ = false;
}

void
MMul::collectRangeInfoPreTrunc()
{
    Range lhsRange(lhs());
    Range rhsRange(rhs());

    // If lhsRange contains only positive then we can skip negative zero check.
    if (lhsRange.isFiniteNonNegative() && !lhsRange.canBeZero())
        setCanBeNegativeZero(false);

    // Likewise rhsRange.
    if (rhsRange.isFiniteNonNegative() && !rhsRange.canBeZero())
        setCanBeNegativeZero(false);

    // If rhsRange and lhsRange contain Non-negative integers only,
    // We skip negative zero check.
    if (rhsRange.isFiniteNonNegative() && lhsRange.isFiniteNonNegative())
        setCanBeNegativeZero(false);

    //If rhsRange and lhsRange < 0. Then we skip negative zero check.
    if (rhsRange.isFiniteNegative() && lhsRange.isFiniteNegative())
        setCanBeNegativeZero(false);
}

void
MMod::collectRangeInfoPreTrunc()
{
    Range lhsRange(lhs());
    Range rhsRange(rhs());
    if (lhsRange.isFiniteNonNegative())
        canBeNegativeDividend_ = false;
    if (!rhsRange.canBeZero())
        canBeDivideByZero_ = false;

}

void
MToNumberInt32::collectRangeInfoPreTrunc()
{
    Range inputRange(input());
    if (!inputRange.canBeNegativeZero())
        canBeNegativeZero_ = false;
}

void
MBoundsCheck::collectRangeInfoPreTrunc()
{
    Range indexRange(index());
    Range lengthRange(length());
    if (!indexRange.hasInt32LowerBound() || !indexRange.hasInt32UpperBound())
        return;
    if (!lengthRange.hasInt32LowerBound() || lengthRange.canBeNaN())
        return;

    int64_t indexLower = indexRange.lower();
    int64_t indexUpper = indexRange.upper();
    int64_t lengthLower = lengthRange.lower();
    int64_t min = minimum();
    int64_t max = maximum();

    if (indexLower + min >= 0 && indexUpper + max < lengthLower)
        fallible_ = false;
}

void
MBoundsCheckLower::collectRangeInfoPreTrunc()
{
    Range indexRange(index());
    if (indexRange.hasInt32LowerBound() && indexRange.lower() >= minimum_)
        fallible_ = false;
}

void
MCompare::collectRangeInfoPreTrunc()
{
    if (!Range(lhs()).canBeNaN() && !Range(rhs()).canBeNaN())
        operandsAreNeverNaN_ = true;
}

void
MNot::collectRangeInfoPreTrunc()
{
    if (!Range(input()).canBeNaN())
        operandIsNeverNaN_ = true;
}

void
MPowHalf::collectRangeInfoPreTrunc()
{
    Range inputRange(input());
    if (!inputRange.canBeInfiniteOrNaN() || inputRange.hasInt32LowerBound())
        operandIsNeverNegativeInfinity_ = true;
    if (!inputRange.canBeNegativeZero())
        operandIsNeverNegativeZero_ = true;
    if (!inputRange.canBeNaN())
        operandIsNeverNaN_ = true;
}

void
MUrsh::collectRangeInfoPreTrunc()
{
    if (specialization_ == MIRType::Int64)
        return;

    Range lhsRange(lhs()), rhsRange(rhs());

    // As in MUrsh::computeRange(), convert the inputs.
    lhsRange.wrapAroundToInt32();
    rhsRange.wrapAroundToShiftCount();

    // If the most significant bit of our result is always going to be zero,
    // we can optimize by disabling bailout checks for enforcing an int32 range.
    if (lhsRange.lower() >= 0 || rhsRange.lower() >= 1)
        bailoutsDisabled_ = true;
}

static bool
DoesMaskMatchRange(int32_t mask, Range& range)
{
    // Check if range is positive, because the bitand operator in `(-3) & 0xff` can't be
    // eliminated.
    if (range.lower() >= 0) {
        MOZ_ASSERT(range.isInt32());
        // Check that the mask value has all bits set given the range upper bound. Note that the
        // upper bound does not have to be exactly the mask value. For example, consider `x &
        // 0xfff` where `x` is a uint8. That expression can still be optimized to `x`.
        int bits = 1 + FloorLog2(range.upper());
        uint32_t maskNeeded = (bits == 32) ? 0xffffffff : (uint32_t(1) << bits) - 1;
        if ((mask & maskNeeded) == maskNeeded)
            return true;
    }

    return false;
}

void
MBinaryBitwiseInstruction::collectRangeInfoPreTrunc()
{
    Range lhsRange(lhs());
    Range rhsRange(rhs());

    if (lhs()->isConstant() && lhs()->type() == MIRType::Int32 &&
        DoesMaskMatchRange(lhs()->toConstant()->toInt32(), rhsRange))
    {
        maskMatchesRightRange = true;
    }

    if (rhs()->isConstant() && rhs()->type() == MIRType::Int32 &&
        DoesMaskMatchRange(rhs()->toConstant()->toInt32(), lhsRange))
    {
        maskMatchesLeftRange = true;
    }
}

void
MNaNToZero::collectRangeInfoPreTrunc()
{
    Range inputRange(input());

    if (!inputRange.canBeNaN())
        operandIsNeverNaN_ = true;
    if (!inputRange.canBeNegativeZero())
        operandIsNeverNegativeZero_ = true;
}

bool
RangeAnalysis::prepareForUCE(bool* shouldRemoveDeadCode)
{
    *shouldRemoveDeadCode = false;

    for (ReversePostorderIterator iter(graph_.rpoBegin()); iter != graph_.rpoEnd(); iter++) {
        MBasicBlock* block = *iter;

        if (!block->unreachable())
            continue;

        // Filter out unreachable fake entries.
        if (block->numPredecessors() == 0) {
            // Ignore fixup blocks added by the Value Numbering phase, in order
            // to keep the dominator tree as-is when we have OSR Block which are
            // no longer reachable from the main entry point of the graph.
            MOZ_ASSERT(graph_.osrBlock());
            continue;
        }

        MControlInstruction* cond = block->getPredecessor(0)->lastIns();
        if (!cond->isTest())
            continue;

        // Replace the condition of the test control instruction by a constant
        // chosen based which of the successors has the unreachable flag which is
        // added by MBeta::computeRange on its own block.
        MTest* test = cond->toTest();
        MDefinition* condition = test->input();

        // If the false-branch is unreachable, then the test condition must be true.
        // If the true-branch is unreachable, then the test condition must be false.
        MOZ_ASSERT(block == test->ifTrue() || block == test->ifFalse());
        bool value = block == test->ifFalse();
        MConstant* constant = MConstant::New(alloc().fallible(), BooleanValue(value));
        if (!constant)
            return false;

        condition->setGuardRangeBailoutsUnchecked();

        test->block()->insertBefore(test, constant);

        test->replaceOperand(0, constant);
        JitSpew(JitSpew_Range, "Update condition of %d to reflect unreachable branches.",
                test->id());

        *shouldRemoveDeadCode = true;
    }

    return tryRemovingGuards();
}

bool RangeAnalysis::tryRemovingGuards()
{
    MDefinitionVector guards(alloc());

    for (ReversePostorderIterator block = graph_.rpoBegin(); block != graph_.rpoEnd(); block++) {
        for (MDefinitionIterator iter(*block); iter; iter++) {
            if (!iter->isGuardRangeBailouts())
                continue;

            iter->setInWorklist();
            if (!guards.append(*iter))
                return false;
        }
    }

    // Flag all fallible instructions which were indirectly used in the
    // computation of the condition, such that we do not ignore
    // bailout-paths which are used to shrink the input range of the
    // operands of the condition.
    for (size_t i = 0; i < guards.length(); i++) {
        MDefinition* guard = guards[i];

        // If this ins is a guard even without guardRangeBailouts,
        // there is no reason in trying to hoist the guardRangeBailouts check.
        guard->setNotGuardRangeBailouts();
        if (!DeadIfUnused(guard)) {
            guard->setGuardRangeBailouts();
            continue;
        }
        guard->setGuardRangeBailouts();

        if (!guard->isPhi()) {
            if (!guard->range())
                continue;

            // Filter the range of the instruction based on its MIRType.
            Range typeFilteredRange(guard);

            // If the output range is updated by adding the inner range,
            // then the MIRType act as an effectful filter. As we do not know if
            // this filtered Range might change or not the result of the
            // previous comparison, we have to keep this instruction as a guard
            // because it has to bailout in order to restrict the Range to its
            // MIRType.
            if (typeFilteredRange.update(guard->range()))
                continue;
        }

        guard->setNotGuardRangeBailouts();

        // Propagate the guard to its operands.
        for (size_t op = 0, e = guard->numOperands(); op < e; op++) {
            MDefinition* operand = guard->getOperand(op);

            // Already marked.
            if (operand->isInWorklist())
                continue;

            MOZ_ASSERT(!operand->isGuardRangeBailouts());

            operand->setInWorklist();
            operand->setGuardRangeBailouts();
            if (!guards.append(operand))
                return false;
        }
    }

    for (size_t i = 0; i < guards.length(); i++) {
        MDefinition* guard = guards[i];
        guard->setNotInWorklist();
    }

    return true;
}

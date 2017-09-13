/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_RangeAnalysis_h
#define jit_RangeAnalysis_h

#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/IonAnalysis.h"
#include "jit/MIR.h"

// windows.h defines those, which messes with the definitions below.
#undef min
#undef max

namespace js {
namespace jit {

class MBasicBlock;
class MIRGraph;

// An upper bound computed on the number of backedges a loop will take.
// This count only includes backedges taken while running Ion code: for OSR
// loops, this will exclude iterations that executed in the interpreter or in
// baseline compiled code.
struct LoopIterationBound : public TempObject
{
    // Loop for which this bound applies.
    MBasicBlock* header;

    // Test from which this bound was derived; after executing exactly 'bound'
    // times this test will exit the loop. Code in the loop body which this
    // test dominates (will include the backedge) will execute at most 'bound'
    // times. Other code in the loop will execute at most '1 + Max(bound, 0)'
    // times.
    MTest* test;

    // Symbolic bound computed for the number of backedge executions. The terms
    // in this bound are all loop invariant.
    LinearSum boundSum;

    // Linear sum for the number of iterations already executed, at the start
    // of the loop header. This will use loop invariant terms and header phis.
    LinearSum currentSum;

    LoopIterationBound(MBasicBlock* header, MTest* test, LinearSum boundSum, LinearSum currentSum)
      : header(header), test(test),
        boundSum(boundSum), currentSum(currentSum)
    {
    }
};

typedef Vector<LoopIterationBound*, 0, SystemAllocPolicy> LoopIterationBoundVector;

// A symbolic upper or lower bound computed for a term.
struct SymbolicBound : public TempObject
{
  private:
    SymbolicBound(LoopIterationBound* loop, LinearSum sum)
      : loop(loop), sum(sum)
    {
    }

  public:
    // Any loop iteration bound from which this was derived.
    //
    // If non-nullptr, then 'sum' is only valid within the loop body, at
    // points dominated by the loop bound's test (see LoopIterationBound).
    //
    // If nullptr, then 'sum' is always valid.
    LoopIterationBound* loop;

    static SymbolicBound* New(TempAllocator& alloc, LoopIterationBound* loop, LinearSum sum) {
        return new(alloc) SymbolicBound(loop, sum);
    }

    // Computed symbolic bound, see above.
    LinearSum sum;

    void dump(GenericPrinter& out) const;
    void dump() const;
};

class RangeAnalysis
{
  protected:
    bool blockDominates(MBasicBlock* b, MBasicBlock* b2);
    void replaceDominatedUsesWith(MDefinition* orig, MDefinition* dom,
                                  MBasicBlock* block);

  protected:
    MIRGenerator* mir;
    MIRGraph& graph_;

    TempAllocator& alloc() const;

  public:
    RangeAnalysis(MIRGenerator* mir, MIRGraph& graph) :
        mir(mir), graph_(graph) {}
    bool addBetaNodes();
    bool analyze();
    bool addRangeAssertions();
    bool removeBetaNodes();
    bool prepareForUCE(bool* shouldRemoveDeadCode);
    bool tryRemovingGuards();
    bool truncate();

    // Any iteration bounds discovered for loops in the graph.
    LoopIterationBoundVector loopIterationBounds;

  private:
    bool analyzeLoop(MBasicBlock* header);
    LoopIterationBound* analyzeLoopIterationCount(MBasicBlock* header,
                                                  MTest* test, BranchDirection direction);
    void analyzeLoopPhi(MBasicBlock* header, LoopIterationBound* loopBound, MPhi* phi);
    bool tryHoistBoundsCheck(MBasicBlock* header, MBoundsCheck* ins);
};

class Range : public TempObject {
  public:
    // Int32 are signed. INT32_MAX is pow(2,31)-1 and INT32_MIN is -pow(2,31),
    // so the greatest exponent we need is 31.
    static const uint16_t MaxInt32Exponent = 31;

    // UInt32 are unsigned. UINT32_MAX is pow(2,32)-1, so it's the greatest
    // value that has an exponent of 31.
    static const uint16_t MaxUInt32Exponent = 31;

    // Maximal exponenent under which we have no precission loss on double
    // operations. Double has 52 bits of mantissa, so 2^52+1 cannot be
    // represented without loss.
    static const uint16_t MaxTruncatableExponent = mozilla::FloatingPoint<double>::kExponentShift;

    // Maximum exponent for finite values.
    static const uint16_t MaxFiniteExponent = mozilla::FloatingPoint<double>::kExponentBias;

    // An special exponent value representing all non-NaN values. This
    // includes finite values and the infinities.
    static const uint16_t IncludesInfinity = MaxFiniteExponent + 1;

    // An special exponent value representing all possible double-precision
    // values. This includes finite values, the infinities, and NaNs.
    static const uint16_t IncludesInfinityAndNaN = UINT16_MAX;

    // This range class uses int32_t ranges, but has several interfaces which
    // use int64_t, which either holds an int32_t value, or one of the following
    // special values which mean a value which is beyond the int32 range,
    // potentially including infinity or NaN. These special values are
    // guaranteed to compare greater, and less than, respectively, any int32_t
    // value.
    static const int64_t NoInt32UpperBound = int64_t(JSVAL_INT_MAX) + 1;
    static const int64_t NoInt32LowerBound = int64_t(JSVAL_INT_MIN) - 1;

    enum FractionalPartFlag {
        ExcludesFractionalParts = false,
        IncludesFractionalParts = true
    };
    enum NegativeZeroFlag {
        ExcludesNegativeZero = false,
        IncludesNegativeZero = true
    };

  private:
    // Absolute ranges.
    //
    // We represent ranges where the endpoints can be in the set:
    // {-infty} U [INT_MIN, INT_MAX] U {infty}.  A bound of +/-
    // infty means that the value may have overflowed in that
    // direction. When computing the range of an integer
    // instruction, the ranges of the operands can be clamped to
    // [INT_MIN, INT_MAX], since if they had overflowed they would
    // no longer be integers. This is important for optimizations
    // and somewhat subtle.
    //
    // N.B.: All of the operations that compute new ranges based
    // on existing ranges will ignore the hasInt32*Bound_ flags of the
    // input ranges; that is, they implicitly clamp the ranges of
    // the inputs to [INT_MIN, INT_MAX]. Therefore, while our range might
    // be unbounded (and could overflow), when using this information to
    // propagate through other ranges, we disregard this fact; if that code
    // executes, then the overflow did not occur, so we may safely assume
    // that the range is [INT_MIN, INT_MAX] instead.
    //
    // To facilitate this trick, we maintain the invariants that:
    // 1) hasInt32LowerBound_ == false implies lower_ == JSVAL_INT_MIN
    // 2) hasInt32UpperBound_ == false implies upper_ == JSVAL_INT_MAX
    //
    // As a second and less precise range analysis, we represent the maximal
    // exponent taken by a value. The exponent is calculated by taking the
    // absolute value and looking at the position of the highest bit.  All
    // exponent computation have to be over-estimations of the actual result. On
    // the Int32 this over approximation is rectified.

    int32_t lower_;
    int32_t upper_;

    bool hasInt32LowerBound_;
    bool hasInt32UpperBound_;

    FractionalPartFlag canHaveFractionalPart_ : 1;
    NegativeZeroFlag canBeNegativeZero_ : 1;
    uint16_t max_exponent_;

    // Any symbolic lower or upper bound computed for this term.
    const SymbolicBound* symbolicLower_;
    const SymbolicBound* symbolicUpper_;

    // This function simply makes several MOZ_ASSERTs to verify the internal
    // consistency of this range.
    void assertInvariants() const {
        // Basic sanity :).
        MOZ_ASSERT(lower_ <= upper_);

        // When hasInt32LowerBound_ or hasInt32UpperBound_ are false, we set
        // lower_ and upper_ to these specific values as it simplifies the
        // implementation in some places.
        MOZ_ASSERT_IF(!hasInt32LowerBound_, lower_ == JSVAL_INT_MIN);
        MOZ_ASSERT_IF(!hasInt32UpperBound_, upper_ == JSVAL_INT_MAX);

        // max_exponent_ must be one of three possible things.
        MOZ_ASSERT(max_exponent_ <= MaxFiniteExponent ||
                   max_exponent_ == IncludesInfinity ||
                   max_exponent_ == IncludesInfinityAndNaN);

        // Forbid the max_exponent_ field from implying better bounds for
        // lower_/upper_ fields. We have to add 1 to the max_exponent_ when
        // canHaveFractionalPart_ is true in order to accomodate
        // fractional offsets. For example, 2147483647.9 is greater than
        // INT32_MAX, so a range containing that value will have
        // hasInt32UpperBound_ set to false, however that value also has
        // exponent 30, which is strictly less than MaxInt32Exponent. For
        // another example, 1.9 has an exponent of 0 but requires upper_ to be
        // at least 2, which has exponent 1.
        mozilla::DebugOnly<uint32_t> adjustedExponent = max_exponent_ +
            (canHaveFractionalPart_ ? 1 : 0);
        MOZ_ASSERT_IF(!hasInt32LowerBound_ || !hasInt32UpperBound_,
                      adjustedExponent >= MaxInt32Exponent);
        MOZ_ASSERT(adjustedExponent >= mozilla::FloorLog2(mozilla::Abs(upper_)));
        MOZ_ASSERT(adjustedExponent >= mozilla::FloorLog2(mozilla::Abs(lower_)));

        // The following are essentially static assertions, but FloorLog2 isn't
        // trivially suitable for constexpr :(.
        MOZ_ASSERT(mozilla::FloorLog2(JSVAL_INT_MIN) == MaxInt32Exponent);
        MOZ_ASSERT(mozilla::FloorLog2(JSVAL_INT_MAX) == 30);
        MOZ_ASSERT(mozilla::FloorLog2(UINT32_MAX) == MaxUInt32Exponent);
        MOZ_ASSERT(mozilla::FloorLog2(0) == 0);
    }

    // Set the lower_ and hasInt32LowerBound_ values.
    void setLowerInit(int64_t x) {
        if (x > JSVAL_INT_MAX) {
            lower_ = JSVAL_INT_MAX;
            hasInt32LowerBound_ = true;
        } else if (x < JSVAL_INT_MIN) {
            lower_ = JSVAL_INT_MIN;
            hasInt32LowerBound_ = false;
        } else {
            lower_ = int32_t(x);
            hasInt32LowerBound_ = true;
        }
    }
    // Set the upper_ and hasInt32UpperBound_ values.
    void setUpperInit(int64_t x) {
        if (x > JSVAL_INT_MAX) {
            upper_ = JSVAL_INT_MAX;
            hasInt32UpperBound_ = false;
        } else if (x < JSVAL_INT_MIN) {
            upper_ = JSVAL_INT_MIN;
            hasInt32UpperBound_ = true;
        } else {
            upper_ = int32_t(x);
            hasInt32UpperBound_ = true;
        }
    }

    // Compute the least exponent value that would be compatible with the
    // values of lower() and upper().
    //
    // Note:
    //     exponent of JSVAL_INT_MIN == 31
    //     exponent of JSVAL_INT_MAX == 30
    uint16_t exponentImpliedByInt32Bounds() const {
         // The number of bits needed to encode |max| is the power of 2 plus one.
         uint32_t max = Max(mozilla::Abs(lower()), mozilla::Abs(upper()));
         uint16_t result = mozilla::FloorLog2(max);
         MOZ_ASSERT(result == (max == 0 ? 0 : mozilla::ExponentComponent(double(max))));
         return result;
    }

    // When converting a range which contains fractional values to a range
    // containing only integers, the old max_exponent_ value may imply a better
    // lower and/or upper bound than was previously available, because they no
    // longer need to be conservative about fractional offsets and the ends of
    // the range.
    //
    // Given an exponent value and pointers to the lower and upper bound values,
    // this function refines the lower and upper bound values to the tighest
    // bound for integer values implied by the exponent.
    static void refineInt32BoundsByExponent(uint16_t e,
                                            int32_t* l, bool* lb,
                                            int32_t* h, bool* hb)
    {
       if (e < MaxInt32Exponent) {
           // pow(2, max_exponent_+1)-1 to compute a maximum absolute value.
           int32_t limit = (uint32_t(1) << (e + 1)) - 1;
           *h = Min(*h, limit);
           *l = Max(*l, -limit);
           *hb = true;
           *lb = true;
       }
    }

    // If the value of any of the fields implies a stronger possible value for
    // any other field, update that field to the stronger value. The range must
    // be completely valid before and it is guaranteed to be kept valid.
    void optimize() {
        assertInvariants();

        if (hasInt32Bounds()) {
            // Examine lower() and upper(), and if they imply a better exponent
            // bound than max_exponent_, set that value as the new
            // max_exponent_.
            uint16_t newExponent = exponentImpliedByInt32Bounds();
            if (newExponent < max_exponent_) {
                max_exponent_ = newExponent;
                assertInvariants();
            }

            // If we have a completely precise range, the value is an integer,
            // since we can only represent integers.
            if (canHaveFractionalPart_ && lower_ == upper_) {
                canHaveFractionalPart_ = ExcludesFractionalParts;
                assertInvariants();
            }
        }

        // If the range doesn't include zero, it doesn't include negative zero.
        if (canBeNegativeZero_ && !canBeZero()) {
            canBeNegativeZero_ = ExcludesNegativeZero;
            assertInvariants();
        }
    }

    // Set the range fields to the given raw values.
    void rawInitialize(int32_t l, bool lb, int32_t h, bool hb,
                       FractionalPartFlag canHaveFractionalPart,
                       NegativeZeroFlag canBeNegativeZero,
                       uint16_t e)
    {
        lower_ = l;
        upper_ = h;
        hasInt32LowerBound_ = lb;
        hasInt32UpperBound_ = hb;
        canHaveFractionalPart_ = canHaveFractionalPart;
        canBeNegativeZero_ = canBeNegativeZero;
        max_exponent_ = e;
        optimize();
    }

    // Construct a range from the given raw values.
    Range(int32_t l, bool lb, int32_t h, bool hb,
          FractionalPartFlag canHaveFractionalPart,
          NegativeZeroFlag canBeNegativeZero,
          uint16_t e)
      : symbolicLower_(nullptr),
        symbolicUpper_(nullptr)
     {
        rawInitialize(l, lb, h, hb, canHaveFractionalPart, canBeNegativeZero, e);
     }

  public:
    Range()
      : symbolicLower_(nullptr),
        symbolicUpper_(nullptr)
    {
        setUnknown();
    }

    Range(int64_t l, int64_t h,
          FractionalPartFlag canHaveFractionalPart,
          NegativeZeroFlag canBeNegativeZero,
          uint16_t e)
      : symbolicLower_(nullptr),
        symbolicUpper_(nullptr)
    {
        set(l, h, canHaveFractionalPart, canBeNegativeZero, e);
    }

    Range(const Range& other)
      : lower_(other.lower_),
        upper_(other.upper_),
        hasInt32LowerBound_(other.hasInt32LowerBound_),
        hasInt32UpperBound_(other.hasInt32UpperBound_),
        canHaveFractionalPart_(other.canHaveFractionalPart_),
        canBeNegativeZero_(other.canBeNegativeZero_),
        max_exponent_(other.max_exponent_),
        symbolicLower_(nullptr),
        symbolicUpper_(nullptr)
    {
        assertInvariants();
    }

    // Construct a range from the given MDefinition. This differs from the
    // MDefinition's range() method in that it describes the range of values
    // *after* any bailout checks.
    explicit Range(const MDefinition* def);

    static Range* NewInt32Range(TempAllocator& alloc, int32_t l, int32_t h) {
        return new(alloc) Range(l, h, ExcludesFractionalParts, ExcludesNegativeZero, MaxInt32Exponent);
    }

    // Construct an int32 range containing just i. This is just a convenience
    // wrapper around NewInt32Range.
    static Range* NewInt32SingletonRange(TempAllocator& alloc, int32_t i) {
        return NewInt32Range(alloc, i, i);
    }

    static Range* NewUInt32Range(TempAllocator& alloc, uint32_t l, uint32_t h) {
        // For now, just pass them to the constructor as int64_t values.
        // They'll become unbounded if they're not in the int32_t range.
        return new(alloc) Range(l, h, ExcludesFractionalParts, ExcludesNegativeZero, MaxUInt32Exponent);
    }

    // Construct a range containing values >= l and <= h. Note that this
    // function treats negative zero as equal to zero, as >= and <= do. If the
    // range includes zero, it is assumed to include negative zero too.
    static Range* NewDoubleRange(TempAllocator& alloc, double l, double h) {
        if (mozilla::IsNaN(l) && mozilla::IsNaN(h))
            return nullptr;

        Range* r = new(alloc) Range();
        r->setDouble(l, h);
        return r;
    }

    // Construct the strictest possible range containing d, or null if d is NaN.
    // This function treats negative zero as distinct from zero, since this
    // makes the strictest possible range containin zero a range which
    // contains one value rather than two.
    static Range* NewDoubleSingletonRange(TempAllocator& alloc, double d) {
        if (mozilla::IsNaN(d))
            return nullptr;

        Range* r = new(alloc) Range();
        r->setDoubleSingleton(d);
        return r;
    }

    void dump(GenericPrinter& out) const;
    void dump() const;
    bool update(const Range* other);

    // Unlike the other operations, unionWith is an in-place
    // modification. This is to avoid a bunch of useless extra
    // copying when chaining together unions when handling Phi
    // nodes.
    void unionWith(const Range* other);
    static Range* intersect(TempAllocator& alloc, const Range* lhs, const Range* rhs,
                             bool* emptyRange);
    static Range* add(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* sub(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* mul(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* and_(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* or_(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* xor_(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* not_(TempAllocator& alloc, const Range* op);
    static Range* lsh(TempAllocator& alloc, const Range* lhs, int32_t c);
    static Range* rsh(TempAllocator& alloc, const Range* lhs, int32_t c);
    static Range* ursh(TempAllocator& alloc, const Range* lhs, int32_t c);
    static Range* lsh(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* rsh(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* ursh(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* abs(TempAllocator& alloc, const Range* op);
    static Range* min(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* max(TempAllocator& alloc, const Range* lhs, const Range* rhs);
    static Range* floor(TempAllocator& alloc, const Range* op);
    static Range* ceil(TempAllocator& alloc, const Range* op);
    static Range* sign(TempAllocator& alloc, const Range* op);

    static bool negativeZeroMul(const Range* lhs, const Range* rhs);

    bool isUnknownInt32() const {
        return isInt32() && lower() == INT32_MIN && upper() == INT32_MAX;
    }

    bool isUnknown() const {
        return !hasInt32LowerBound_ &&
               !hasInt32UpperBound_ &&
               canHaveFractionalPart_ &&
               canBeNegativeZero_ &&
               max_exponent_ == IncludesInfinityAndNaN;
    }

    bool hasInt32LowerBound() const {
        return hasInt32LowerBound_;
    }
    bool hasInt32UpperBound() const {
        return hasInt32UpperBound_;
    }

    // Test whether the value is known to be within [INT32_MIN,INT32_MAX].
    // Note that this does not necessarily mean the value is an integer.
    bool hasInt32Bounds() const {
        return hasInt32LowerBound() && hasInt32UpperBound();
    }

    // Test whether the value is known to be representable as an int32.
    bool isInt32() const {
        return hasInt32Bounds() &&
               !canHaveFractionalPart_ &&
               !canBeNegativeZero_;
    }

    // Test whether the given value is known to be either 0 or 1.
    bool isBoolean() const {
        return lower() >= 0 && upper() <= 1 &&
               !canHaveFractionalPart_ &&
               !canBeNegativeZero_;
    }

    bool canHaveRoundingErrors() const {
        return canHaveFractionalPart_ ||
               canBeNegativeZero_ ||
               max_exponent_ >= MaxTruncatableExponent;
    }

    // Test if an integer x belongs to the range.
    bool contains(int32_t x) const {
        return x >= lower_ && x <= upper_;
    }

    // Test whether the range contains zero (of either sign).
    bool canBeZero() const {
        return contains(0);
    }

    // Test whether the range contains NaN values.
    bool canBeNaN() const {
        return max_exponent_ == IncludesInfinityAndNaN;
    }

    // Test whether the range contains infinities or NaN values.
    bool canBeInfiniteOrNaN() const {
        return max_exponent_ >= IncludesInfinity;
    }

    FractionalPartFlag canHaveFractionalPart() const {
        return canHaveFractionalPart_;
    }

    NegativeZeroFlag canBeNegativeZero() const {
        return canBeNegativeZero_;
    }

    uint16_t exponent() const {
        MOZ_ASSERT(!canBeInfiniteOrNaN());
        return max_exponent_;
    }

    uint16_t numBits() const {
        return exponent() + 1; // 2^0 -> 1
    }

    // Return the lower bound. Asserts that the value has an int32 bound.
    int32_t lower() const {
        MOZ_ASSERT(hasInt32LowerBound());
        return lower_;
    }

    // Return the upper bound. Asserts that the value has an int32 bound.
    int32_t upper() const {
        MOZ_ASSERT(hasInt32UpperBound());
        return upper_;
    }

    // Test whether all values in this range can are finite and negative.
    bool isFiniteNegative() const {
        return upper_ < 0 && !canBeInfiniteOrNaN();
    }

    // Test whether all values in this range can are finite and non-negative.
    bool isFiniteNonNegative() const {
        return lower_ >= 0 && !canBeInfiniteOrNaN();
    }

    // Test whether a value in this range can possibly be a finite
    // negative value. Note that "negative zero" is not considered negative.
    bool canBeFiniteNegative() const {
        return lower_ < 0;
    }

    // Test whether a value in this range can possibly be a finite
    // non-negative value.
    bool canBeFiniteNonNegative() const {
        return upper_ >= 0;
    }

    // Test whether a value in this range can have the sign bit set (not
    // counting NaN, where the sign bit is meaningless).
    bool canHaveSignBitSet() const {
        return !hasInt32LowerBound() || canBeFiniteNegative() || canBeNegativeZero();
    }

    // Set this range to have a lower bound not less than x.
    void refineLower(int32_t x) {
        assertInvariants();
        hasInt32LowerBound_ = true;
        lower_ = Max(lower_, x);
        optimize();
    }

    // Set this range to have an upper bound not greater than x.
    void refineUpper(int32_t x) {
        assertInvariants();
        hasInt32UpperBound_ = true;
        upper_ = Min(upper_, x);
        optimize();
    }

    // Set this range to exclude negative zero.
    void refineToExcludeNegativeZero() {
        assertInvariants();
        canBeNegativeZero_ = ExcludesNegativeZero;
        optimize();
    }

    void setInt32(int32_t l, int32_t h) {
        hasInt32LowerBound_ = true;
        hasInt32UpperBound_ = true;
        lower_ = l;
        upper_ = h;
        canHaveFractionalPart_ = ExcludesFractionalParts;
        canBeNegativeZero_ = ExcludesNegativeZero;
        max_exponent_ = exponentImpliedByInt32Bounds();
        assertInvariants();
    }

    // Set this range to include values >= l and <= h. Note that this
    // function treats negative zero as equal to zero, as >= and <= do. If the
    // range includes zero, it is assumed to include negative zero too.
    void setDouble(double l, double h);

    // Set this range to the narrowest possible range containing d.
    // This function treats negative zero as distinct from zero, since this
    // makes the narrowest possible range containin zero a range which
    // contains one value rather than two.
    void setDoubleSingleton(double d);

    void setUnknown() {
        set(NoInt32LowerBound, NoInt32UpperBound,
            IncludesFractionalParts,
            IncludesNegativeZero,
            IncludesInfinityAndNaN);
        MOZ_ASSERT(isUnknown());
    }

    void set(int64_t l, int64_t h,
             FractionalPartFlag canHaveFractionalPart,
             NegativeZeroFlag canBeNegativeZero,
             uint16_t e)
    {
        max_exponent_ = e;
        canHaveFractionalPart_ = canHaveFractionalPart;
        canBeNegativeZero_ = canBeNegativeZero;
        setLowerInit(l);
        setUpperInit(h);
        optimize();
    }

    // Make the lower end of this range at least INT32_MIN, and make
    // the upper end of this range at most INT32_MAX.
    void clampToInt32();

    // If this range exceeds int32_t range, at either or both ends, change
    // it to int32_t range.  Otherwise do nothing.
    void wrapAroundToInt32();

    // If this range exceeds [0, 32) range, at either or both ends, change
    // it to the [0, 32) range.  Otherwise do nothing.
    void wrapAroundToShiftCount();

    // If this range exceeds [0, 1] range, at either or both ends, change
    // it to the [0, 1] range.  Otherwise do nothing.
    void wrapAroundToBoolean();

    const SymbolicBound* symbolicLower() const {
        return symbolicLower_;
    }
    const SymbolicBound* symbolicUpper() const {
        return symbolicUpper_;
    }

    void setSymbolicLower(SymbolicBound* bound) {
        symbolicLower_ = bound;
    }
    void setSymbolicUpper(SymbolicBound* bound) {
        symbolicUpper_ = bound;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_RangeAnalysis_h */

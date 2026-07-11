// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/record_id_range_list.h"

#include "mongo/db/query/record_id_range_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;


// ---------------------------------------------------------------------------
// RecordIdRangeList construction and isUnbounded
// ---------------------------------------------------------------------------

TEST(RecordIdRangeListTest, DefaultConstruction_IsUnbounded) {
    RecordIdRangeList list;
    ASSERT_TRUE(list.isUnbounded());
    ASSERT_EQ(list.getRanges().size(), 1u);
    assertRangeUnbounded(list.getRanges().front());
}

TEST(RecordIdRangeListTest, SingleRangeConstruction) {
    auto r = makeRange(1, true, 5, false);
    RecordIdRangeList list{r};
    ASSERT_FALSE(list.isUnbounded());
    ASSERT_EQ(list.getRanges().size(), 1u);
    assertRange(list.getRanges()[0], 1, true, 5, false);
}

TEST(RecordIdRangeListTest, SingleUnboundedRangeConstruction_IsUnbounded) {
    RecordIdRange r;  // (-∞, +∞)
    RecordIdRangeList list{r};
    ASSERT_TRUE(list.isUnbounded());
}

// ---------------------------------------------------------------------------
// RecordIdRangeList::makeUnion
// ---------------------------------------------------------------------------

TEST(MakeUnionRangeListTest, EmptyInput_ReturnsEmptyList) {
    auto result = RecordIdRangeList::makeUnion(std::vector<RecordIdRange>{});
    ASSERT_TRUE(result.getRanges().empty());
    ASSERT_TRUE(result.isEmpty());
    ASSERT_FALSE(result.isUnbounded());
}

TEST(MakeUnionRangeListTest, SingleRange_NoMerge) {
    auto result = RecordIdRangeList::makeUnion({makeRange(2, true, 6, false)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    ASSERT_FALSE(result.isEmpty());
    assertRange(result.getRanges()[0], 2, true, 6, false);
}

TEST(MakeUnionRangeListTest, TwoDisjointRanges_NotMerged) {
    // [1, 3) and [5, 8]: gap between 3 and 5 → two separate ranges
    auto result =
        RecordIdRangeList::makeUnion({makeRange(1, true, 3, false), makeRange(5, true, 8, true)});
    ASSERT_EQ(result.getRanges().size(), 2u);
    assertRange(result.getRanges()[0], 1, true, 3, false);
    assertRange(result.getRanges()[1], 5, true, 8, true);
}

TEST(MakeUnionRangeListTest, TwoOverlappingRanges_Merged) {
    // [1, 5) and [3, 8] overlap → [1, 8]
    auto result =
        RecordIdRangeList::makeUnion({makeRange(1, true, 5, false), makeRange(3, true, 8, true)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRange(result.getRanges()[0], 1, true, 8, true);
}

TEST(MakeUnionRangeListTest, SortedBeforeMerge_UnsortedInput) {
    // Input in reverse order: [5, 8], [1, 3) → should sort to [1,3), [5,8]
    auto result =
        RecordIdRangeList::makeUnion({makeRange(5, true, 8, true), makeRange(1, true, 3, false)});
    ASSERT_EQ(result.getRanges().size(), 2u);
    assertRange(result.getRanges()[0], 1, true, 3, false);
    assertRange(result.getRanges()[1], 5, true, 8, true);
}

TEST(MakeUnionRangeListTest, AdjacentRanges_Merged) {
    // [1, 3) and [3, 5] touch at 3 → [1, 5]
    auto result =
        RecordIdRangeList::makeUnion({makeRange(1, true, 3, false), makeRange(3, true, 5, true)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRange(result.getRanges()[0], 1, true, 5, true);
}

TEST(MakeUnionRangeListTest, BothExclusiveAtSamePoint_NotMerged) {
    // [1, 3) and (3, 5]: gap at exactly 3 (neither range includes 3)
    auto result =
        RecordIdRangeList::makeUnion({makeRange(1, true, 3, false), makeRange(3, false, 5, true)});
    ASSERT_EQ(result.getRanges().size(), 2u);
    assertRange(result.getRanges()[0], 1, true, 3, false);
    assertRange(result.getRanges()[1], 3, false, 5, true);
}

TEST(MakeUnionRangeListTest, PointRanges_AllSameValue_MergeToOne) {
    // [3,3], [3,3], [3,3] → [3,3]
    auto result = RecordIdRangeList::makeUnion(
        {makeRange(3, true, 3, true), makeRange(3, true, 3, true), makeRange(3, true, 3, true)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRange(result.getRanges()[0], 3, true, 3, true);
}

TEST(MakeUnionRangeListTest, SemiUnboundedStart_MergedCorrectly) {
    // (-∞, 3) and [3, ∞) touch at 3 (exclusive+inclusive) → (-∞, +∞)
    auto r1 = makeRangeMaxOnly(3, false);  // (-∞, 3)
    auto r2 = makeRangeMinOnly(3, true);   // [3, +∞)
    auto result = RecordIdRangeList::makeUnion({r1, r2});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRangeUnbounded(result.getRanges()[0]);
    ASSERT_TRUE(result.isUnbounded());
}

TEST(MakeUnionRangeListTest, SemiUnboundedEnd_LargerRangeAbsorbs) {
    // [3, ∞) ∪ (5, ∞) → [3, ∞)
    auto r1 = makeRangeMinOnly(3, true);   // [3, +∞)
    auto r2 = makeRangeMinOnly(5, false);  // (5, +∞)
    auto result = RecordIdRangeList::makeUnion({r1, r2});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRangeMinOnly(result.getRanges()[0], 3, true);
    ASSERT_FALSE(result.isUnbounded());
}

TEST(MakeUnionRangeListTest, MultipleSemiUnboundedRanges_PreserveSemiUnboundedInvariant) {
    // (-∞, 5] and [7, ∞) → two ranges, first has no min, last has no max
    auto r1 = makeRangeMaxOnly(5, true);  // (-∞, 5]
    auto r2 = makeRangeMinOnly(7, true);  // [7, +∞)
    auto result = RecordIdRangeList::makeUnion({r1, r2});
    ASSERT_EQ(result.getRanges().size(), 2u);
    assertRangeMaxOnly(result.getRanges()[0], 5, true);
    assertRangeMinOnly(result.getRanges()[1], 7, true);
    ASSERT_FALSE(result.isUnbounded());
}

TEST(MakeUnionRangeListTest, ThreeRanges_MiddleAbsorbed) {
    // [1,3], [2,4], [5,7] → [1,4] and [5,7]
    auto result = RecordIdRangeList::makeUnion(
        {makeRange(1, true, 3, true), makeRange(2, true, 4, true), makeRange(5, true, 7, true)});
    ASSERT_EQ(result.getRanges().size(), 2u);
    assertRange(result.getRanges()[0], 1, true, 4, true);
    assertRange(result.getRanges()[1], 5, true, 7, true);
}

// ---------------------------------------------------------------------------
// RecordIdRangeList::unite
// ---------------------------------------------------------------------------

TEST(MakeUnionRangeListFromListsTest, EmptyInput_ReturnsEmptyList) {
    auto result = RecordIdRangeList::unite(std::vector<RecordIdRangeList>{});
    ASSERT_TRUE(result.isEmpty());
    ASSERT_FALSE(result.isUnbounded());
}

TEST(MakeUnionRangeListFromListsTest, TwoLists_UnionedAndMerged) {
    // List A: [1,3], [7,9]
    // List B: [2,5], [8,10]
    // Union: [1,5], [7,10]
    RecordIdRangeList a =
        RecordIdRangeList::makeUnion({makeRange(1, true, 3, true), makeRange(7, true, 9, true)});
    RecordIdRangeList b =
        RecordIdRangeList::makeUnion({makeRange(2, true, 5, true), makeRange(8, true, 10, true)});
    auto result = RecordIdRangeList::unite({std::move(a), std::move(b)});
    ASSERT_EQ(result.getRanges().size(), 2u);
    assertRange(result.getRanges()[0], 1, true, 5, true);
    assertRange(result.getRanges()[1], 7, true, 10, true);
}

TEST(MakeUnionRangeListFromListsTest, SingleList_PassThrough) {
    RecordIdRangeList a = RecordIdRangeList::makeUnion({makeRange(1, true, 5, false)});
    auto result = RecordIdRangeList::unite({std::move(a)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRange(result.getRanges()[0], 1, true, 5, false);
}

// ---------------------------------------------------------------------------
// RecordIdRangeList::intersect
// ---------------------------------------------------------------------------

TEST(IntersectRangeListsTest, EmptyInput_ReturnsUnbounded) {
    auto result = RecordIdRangeList::intersect({});
    ASSERT_TRUE(result.isUnbounded());
}

TEST(IntersectRangeListsTest, SingleList_IsIdentity) {
    RecordIdRangeList a = RecordIdRangeList::makeUnion({makeRange(2, true, 8, true)});
    auto result = RecordIdRangeList::intersect({std::move(a)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRange(result.getRanges()[0], 2, true, 8, true);
}

TEST(IntersectRangeListsTest, TwoLists_OverlappingRanges) {
    // [1, 5] ∩ [3, 8] = [3, 5]
    RecordIdRangeList a{makeRange(1, true, 5, true)};
    RecordIdRangeList b{makeRange(3, true, 8, true)};
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(b)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRange(result.getRanges()[0], 3, true, 5, true);
}

TEST(IntersectRangeListsTest, TwoLists_Disjoint_ReturnsEmptyList) {
    // [1, 3] ∩ [5, 8] = ∅
    RecordIdRangeList a{makeRange(1, true, 3, true)};
    RecordIdRangeList b{makeRange(5, true, 8, true)};
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(b)});
    ASSERT_TRUE(result.isEmpty());
}

TEST(IntersectRangeListsTest, TwoLists_TouchingExclusive_ReturnsEmptyList) {
    // [1, 3) ∩ [3, 8] = ∅  (3 is excluded by the first range)
    RecordIdRangeList a{makeRange(1, true, 3, false)};
    RecordIdRangeList b{makeRange(3, true, 8, true)};
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(b)});
    ASSERT_TRUE(result.isEmpty());
}

TEST(IntersectRangeListsTest, MultiRangeLists_ProducesMultipleIntersections) {
    // A: [1,5), [7,10]
    // B: [2,3], [4,8), [9,11]
    // Expected intersections:
    //   [1,5) ∩ [2,3] = [2,3]
    //   [1,5) ∩ [4,8) = [4,5)
    //   [7,10] ∩ [4,8) = [7,8)
    //   [7,10] ∩ [9,11] = [9,10]
    // Result: [2,3], [4,5), [7,8), [9,10]
    RecordIdRangeList a =
        RecordIdRangeList::makeUnion({makeRange(1, true, 5, false), makeRange(7, true, 10, true)});
    RecordIdRangeList b = RecordIdRangeList::makeUnion(
        {makeRange(2, true, 3, true), makeRange(4, true, 8, false), makeRange(9, true, 11, true)});
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(b)});
    ASSERT_EQ(result.getRanges().size(), 4u);
    assertRange(result.getRanges()[0], 2, true, 3, true);
    assertRange(result.getRanges()[1], 4, true, 5, false);
    assertRange(result.getRanges()[2], 7, true, 8, false);
    assertRange(result.getRanges()[3], 9, true, 10, true);
}

TEST(IntersectRangeListsTest, IntersectWithUnbounded_IsIdentity) {
    // [2, 6] ∩ (-∞, +∞) = [2, 6]
    RecordIdRangeList a{makeRange(2, true, 6, true)};
    RecordIdRangeList unbounded;  // (-∞, +∞)
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(unbounded)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRange(result.getRanges()[0], 2, true, 6, true);
}

TEST(IntersectRangeListsTest, IntersectWithEmpty_ReturnsEmpty) {
    // [2, 6] ∩ ∅ = ∅
    RecordIdRangeList a{makeRange(2, true, 6, true)};
    RecordIdRangeList emptyList = RecordIdRangeList::makeUnion(std::vector<RecordIdRange>{});
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(emptyList)});
    ASSERT_TRUE(result.isEmpty());
}

TEST(IntersectRangeListsTest, ThreeLists) {
    // [1,10] ∩ [2,8] ∩ [3,6] = [3,6]
    RecordIdRangeList a{makeRange(1, true, 10, true)};
    RecordIdRangeList b{makeRange(2, true, 8, true)};
    RecordIdRangeList c{makeRange(3, true, 6, true)};
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(b), std::move(c)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRange(result.getRanges()[0], 3, true, 6, true);
}

TEST(IntersectRangeListsTest, ThreeLists_MultipleOutputRanges) {
    // A: [[1,5], [8,12]]
    // B: [[2,4], [6,10]]
    // C: [[3,7], [9,11]]
    //
    // Expected: [[3,4], [9,10]]
    RecordIdRangeList a =
        RecordIdRangeList::makeUnion({makeRange(1, true, 5, true), makeRange(8, true, 12, true)});
    RecordIdRangeList b =
        RecordIdRangeList::makeUnion({makeRange(2, true, 4, true), makeRange(6, true, 10, true)});
    RecordIdRangeList c =
        RecordIdRangeList::makeUnion({makeRange(3, true, 7, true), makeRange(9, true, 11, true)});
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(b), std::move(c)});
    ASSERT_EQ(result.getRanges().size(), 2u);
    assertRange(result.getRanges()[0], 3, true, 4, true);
    assertRange(result.getRanges()[1], 9, true, 10, true);
}

TEST(IntersectRangeListsTest, OneWideRangeSubsumingManyNarrow) {
    // A: [[0,10]]              — one wide range
    // B: [[1,2], [3,4], [5,6]] — three narrow ranges
    // A subsumes B entirely, so the result is B.
    RecordIdRangeList a{makeRange(0, true, 10, true)};
    RecordIdRangeList b = RecordIdRangeList::makeUnion(
        {makeRange(1, true, 2, true), makeRange(3, true, 4, true), makeRange(5, true, 6, true)});
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(b)});
    ASSERT_EQ(result.getRanges().size(), 3u);
    assertRange(result.getRanges()[0], 1, true, 2, true);
    assertRange(result.getRanges()[1], 3, true, 4, true);
    assertRange(result.getRanges()[2], 5, true, 6, true);
}

TEST(IntersectRangeListsTest, AdvanceHiKeepLo_TwoLists) {
    // A: [1,3], [5,7]
    // B: [6,8]
    // A ∩ B = [6,7]
    RecordIdRangeList a =
        RecordIdRangeList::makeUnion({makeRange(1, true, 3, true), makeRange(5, true, 7, true)});
    RecordIdRangeList b{makeRange(6, true, 8, true)};
    auto result = RecordIdRangeList::intersect({std::move(a), std::move(b)});
    ASSERT_EQ(result.getRanges().size(), 1u);
    assertRange(result.getRanges()[0], 6, true, 7, true);
}

// ---------------------------------------------------------------------------
// outerBounds
// ---------------------------------------------------------------------------

TEST(OuterBoundsTest, Unbounded_ReturnsFullyOpenRange) {
    RecordIdRangeList list;  // (-∞, +∞)
    assertRangeUnbounded(list.outerBounds());
}

TEST(OuterBoundsTest, SingleFiniteRange_MatchesRange) {
    RecordIdRangeList list{makeRange(2, false, 9, true)};
    assertRange(list.outerBounds(), 2, false, 9, true);
}

TEST(OuterBoundsTest, MultipleRanges_SpansOuterBoundsExclusiveStart) {
    // (1,3] and [5,8] → outer bounds = (1,8]
    auto list =
        RecordIdRangeList::makeUnion({makeRange(1, false, 3, true), makeRange(5, true, 8, true)});
    assertRange(list.outerBounds(), 1, false, 8, true);
}

TEST(OuterBoundsTest, MultipleRanges_SpansOuterBoundsExclusiveEnd) {
    // [1,3] and [5,8) → outer bounds = [1,8)
    auto list =
        RecordIdRangeList::makeUnion({makeRange(1, true, 3, true), makeRange(5, true, 8, false)});
    assertRange(list.outerBounds(), 1, true, 8, false);
}

TEST(OuterBoundsTest, EmptyList_ReturnsEmptyRange) {
    auto list = RecordIdRangeList::makeUnion(std::vector<RecordIdRange>{});
    ASSERT_TRUE(list.isEmpty());
    auto outer = list.outerBounds();
    ASSERT_TRUE(outer.isEmpty());
}

TEST(OuterBoundsTest, SemiUnboundedList_PreservesOpenEnds) {
    // (-∞, 5] and [7, +∞)
    auto r1 = makeRangeMaxOnly(5, true);
    auto r2 = makeRangeMinOnly(7, true);
    auto list = RecordIdRangeList::makeUnion({r1, r2});
    assertRangeUnbounded(list.outerBounds());  // outer: (-∞, +∞)
}

}  // namespace

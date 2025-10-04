/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/query/compiler/physical_model/query_solution/eof_node_type.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/wildcard_test_utils.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <utility>

#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Output stream operator for FieldAvailability values is required by ASSERT_ macros in tests.
 */
std::ostream& operator<<(std::ostream& os, FieldAvailability value) {
    switch (value) {
        case FieldAvailability::kNotProvided:
            return os << "NotProvided";
        case FieldAvailability::kHashedValueProvided:
            return os << "HashedValueProvided";
        case FieldAvailability::kFullyProvided:
            return os << "FullyProvided";
    }
    MONGO_UNREACHABLE;
}

/**
 * Output stream operator for ProvidedSortSet instances is required by ASSERT_ macros in tests.
 */
std::ostream& operator<<(std::ostream& os, const ProvidedSortSet& value) {
    return os << value.debugString();
}

/**
 * Equality operator for ProvidedSortSet instances is required by ASSERT_EQ macros in tests.
 * This operator uses 'BSONObj::woCompare()' method for comparing base sort patterns.
 */
bool operator==(const ProvidedSortSet& lhs, const ProvidedSortSet& rhs) {
    return (lhs.getIgnoredFields() == rhs.getIgnoredFields() &&
            lhs.getBaseSortPattern().woCompare(rhs.getBaseSortPattern()) == 0);
}

/**
 * Non-equality operator for ProvidedSortSet instances is required by ASSERT_NE macros in tests.
 * This operator uses 'BSONObj::woCompare()' method for comparing base sort patterns.
 */
bool operator!=(const ProvidedSortSet& lhs, const ProvidedSortSet& rhs) {
    return !(lhs == rhs);
}

}  // namespace mongo

namespace {

using namespace mongo;
/**
 * Make a minimal IndexEntry from just a key pattern. A dummy name will be added.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexConfig::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            nullptr,
            {},
            nullptr,
            nullptr};
}

void assertNamespaceVectorsAreEqual(const std::vector<NamespaceStringOrUUID>& secondaryNssVector,
                                    const std::vector<NamespaceStringOrUUID>& expectedNssVector) {
    ASSERT_EQ(secondaryNssVector.size(), expectedNssVector.size());
    for (size_t i = 0; i < secondaryNssVector.size(); ++i) {
        ASSERT(secondaryNssVector[i].isNamespaceString());
        ASSERT(expectedNssVector[i].isNamespaceString());
        ASSERT_EQ(secondaryNssVector[i].nss(), expectedNssVector[i].nss());
    }
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Min: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Max: {a: 1, b: 1, c: 1, d: 1, e: 1}
TEST(QuerySolutionTest, SimpleRangeAllEqual) {
    IndexScanNode node{
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};
    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    node.bounds.endKey = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    node.computeProperties();

    // Expected sort orders
    ASSERT_EQ(node.providedSorts(), ProvidedSortSet(BSONObj(), {"a", "b", "c", "d", "e"}));

    ASSERT(node.providedSorts().contains(
        BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << -1 << "c" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << -1 << "b" << -1)));
    ASSERT(node.providedSorts().contains(BSON("d" << 1)));
    ASSERT(node.providedSorts().contains(BSON("b" << 1 << "c" << 1 << "e" << 1 << "d" << -1)));
    ASSERT(node.providedSorts().contains(BSON("c" << -1 << "b" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("b" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("e" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Min: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Max: {a: 2, b: 2, c: 2, d: 2, e: 2}
TEST(QuerySolutionTest, SimpleRangeNoneEqual) {
    IndexScanNode node{
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};
    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    node.bounds.endKey = BSON("a" << 2 << "b" << 2 << "c" << 2 << "d" << 2 << "e" << 2);
    node.computeProperties();

    // Expected sort orders
    ASSERT_EQ(node.providedSorts(),
              ProvidedSortSet(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1), {}));
    ASSERT(node.providedSorts().contains(
        BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1)));
    ASSERT_FALSE(node.providedSorts().contains(BSON("b" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Min: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Max: {a: 1, b: 1, c: 2, d: 2, e: 2}
TEST(QuerySolutionTest, SimpleRangeSomeEqual) {
    IndexScanNode node{
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};
    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    node.bounds.endKey = BSON("a" << 1 << "b" << 1 << "c" << 2 << "d" << 2 << "e" << 2);
    node.computeProperties();

    // Expected sort orders
    ASSERT_EQ(node.providedSorts(),
              ProvidedSortSet(BSON("c" << 1 << "d" << 1 << "e" << 1), {"a", "b"}));

    ASSERT(node.providedSorts().contains(
        BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1)));
    ASSERT(node.providedSorts().contains(BSON("b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("c" << 1 << "d" << 1)));
    ASSERT(node.providedSorts().contains(BSON("c" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Intervals: a: [1,1], b: [1,1], c: [1,1], d: [1,1], e: [1,1]
TEST(QuerySolutionTest, IntervalListAllPoints) {
    IndexScanNode node{
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(c);

    OrderedIntervalList d{};
    d.name = "d";
    d.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(d);

    OrderedIntervalList e{};
    e.name = "e";
    e.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(e);

    node.computeProperties();

    // Expected internal state of 'ProvidedSortSet'.
    ASSERT_EQ(node.providedSorts(), ProvidedSortSet(BSONObj(), {"a", "b", "c", "d", "e"}));

    // Expected sort orders.
    ASSERT(node.providedSorts().contains(
        BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << -1 << "b" << 1 << "c" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1)));
    ASSERT(node.providedSorts().contains(BSON("b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("d" << 1 << "e" << 1 << "b" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("b" << 1 << "d" << 1 << "a" << 1)));

    // Verify that the order of equality fields is irrelvant.
    ASSERT(node.providedSorts().contains(BSON("a" << -1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("b" << -1 << "d" << -1 << "a" << -1)));
    ASSERT(node.providedSorts().contains(BSON("d" << 1 << "e" << -1)));
    ASSERT(node.providedSorts().contains(BSON("b" << 1 << "c" << -1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << -1)));
}


// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Intervals: a: [1,2], b: [1,2], c: [1,2], d: [1,2], e: [1,2]
TEST(QuerySolutionTest, IntervalListNoPoints) {
    IndexScanNode node{
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    OrderedIntervalList d{};
    d.name = "d";
    d.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(d);

    OrderedIntervalList e{};
    e.name = "e";
    e.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(e);

    node.computeProperties();

    // Expected sort orders
    ASSERT_EQ(node.providedSorts(),
              ProvidedSortSet(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1), {}));

    ASSERT(node.providedSorts().contains(
        BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Intervals: a: [1,1], b: [1,1], c: [1,2], d: [1,2], e: [1,2]
TEST(QuerySolutionTest, IntervalListSomePoints) {
    IndexScanNode node{
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    OrderedIntervalList d{};
    d.name = "d";
    d.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(d);

    OrderedIntervalList e{};
    e.name = "e";
    e.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(e);

    node.computeProperties();

    // Expected sort orders
    ASSERT_EQ(node.providedSorts(),
              ProvidedSortSet(BSON("c" << 1 << "d" << 1 << "e" << 1), {"a", "b"}));

    ASSERT(node.providedSorts().contains(
        BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1)));
    ASSERT(node.providedSorts().contains(BSON("b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("c" << 1 << "d" << 1)));
    ASSERT(node.providedSorts().contains(BSON("c" << 1)));
    ASSERT(node.providedSorts().contains(BSON("c" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "c" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << -1 << "c" << 1 << "d" << 1 << "b" << 1)));
    ASSERT(node.providedSorts().contains(BSON("c" << 1 << "b" << -1 << "d" << 1 << "a" << 1)));
    ASSERT_FALSE(node.providedSorts().contains(BSON("a" << 1 << "d" << 1)));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesFieldsContainingStrings) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 1), BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilA);

    OrderedIntervalList oilB{};
    oilB.name = "b";
    oilB.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << false << "" << true), BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilB);

    OrderedIntervalList oilC{};
    oilC.name = "c";
    oilC.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << "a"
                                                      << ""
                                                      << "b"),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilC);

    OrderedIntervalList oilD{};
    oilD.name = "d";
    oilD.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << BSON("foo" << 1) << "" << BSON("foo" << 2)),
        BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilD);

    OrderedIntervalList oilE{};
    oilE.name = "e";
    oilE.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << BSON_ARRAY(1 << 2 << 3) << "" << BSON_ARRAY(2 << 3 << 4)),
        BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilE);

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 3U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_FALSE(fields.count("b"));
    ASSERT_TRUE(fields.count("c"));
    ASSERT_TRUE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsFromNonPointBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = BSON("a" << 1 << "b" << 2 << "c" << 3 << "d" << 4 << "e" << 5);
    bounds.endKey = BSON("a" << 1 << "b" << 2 << "c" << 3 << "d" << 5 << "e" << 5);
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_FALSE(fields.count("b"));
    ASSERT_FALSE(fields.count("c"));
    ASSERT_FALSE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsFromStringTypePointBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': 'a', 'c': 3, 'd': 4, 'e': 5}");
    bounds.endKey = bounds.startKey;
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 4U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
    ASSERT_TRUE(fields.count("c"));
    ASSERT_TRUE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsFromArrayTypeBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': [1,2], 'c': 3, 'd': 4, 'e': 5}");
    bounds.endKey = bounds.startKey;
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 4U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
    ASSERT_TRUE(fields.count("c"));
    ASSERT_TRUE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsFromObjectTypeBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': {'foo': 2}, 'c': 3, 'd': 4, 'e': 5}");
    bounds.endKey = bounds.startKey;
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 4U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
    ASSERT_TRUE(fields.count("c"));
    ASSERT_TRUE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsWithExclusiveBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': 1}");
    bounds.endKey = fromjson("{'a': 2, 'b': 2}");
    bounds.boundInclusion = BoundInclusion::kExcludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsWithExclusiveBoundsOnBoundary) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': 1}");
    bounds.endKey = fromjson("{'a': '', 'b': 1}");
    bounds.boundInclusion = BoundInclusion::kExcludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 2U);
    ASSERT_TRUE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesNoStringsWithEmptyExclusiveBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1}");
    bounds.endKey = fromjson("{'a': ''}");
    bounds.boundInclusion = BoundInclusion::kExcludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 0U);
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsWithInclusiveBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1}");
    bounds.endKey = fromjson("{'a': ''}");
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_TRUE(fields.count("a"));
}

TEST(QuerySolutionTest, IndexScanNodeRemovesNonMatchingCollatedFieldsFromSortsOnSimpleBounds) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1);
    node.bounds.endKey = BSON("a" << 2 << "b" << 1);
    node.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    node.computeProperties();

    auto sorts = node.providedSorts();
    ASSERT_EQ(sorts, ProvidedSortSet(BSON("a" << 1), {}));
    ASSERT_TRUE(sorts.contains(BSON("a" << 1)));
}

TEST(QuerySolutionTest, IndexScanNodeGetFieldsWithStringBoundsCorrectlyHandlesEndKeyInclusive) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1);
    node.bounds.endKey = BSON("a" << 1 << "b"
                                  << "");
    node.bounds.boundInclusion = BoundInclusion::kIncludeStartKeyOnly;
    node.computeProperties();

    auto sorts = node.providedSorts();
    ASSERT_EQ(sorts, ProvidedSortSet(BSON("b" << 1), {"a"}));

    ASSERT_TRUE(sorts.contains(BSON("a" << 1)));
    ASSERT_TRUE(sorts.contains(BSON("a" << 1 << "b" << 1)));
    ASSERT_TRUE(sorts.contains(BSON("b" << 1)));

    node.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
    node.computeProperties();

    sorts = node.providedSorts();
    ASSERT_EQ(sorts, ProvidedSortSet(BSONObj(), {"a"}));

    ASSERT_TRUE(sorts.contains(BSON("a" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1)));
}

// Index: {a: 1}
// Bounds: [MINKEY, MAXKEY]
TEST(QuerySolutionTest, IndexScanNodeRemovesCollatedFieldsFromSortsIfCollationDifferent) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    node.computeProperties();

    auto sorts = node.providedSorts();
    ASSERT_EQ(sorts, kEmptySet);
    ASSERT_FALSE(sorts.contains(BSON("a" << 1)));
}

TEST(QuerySolutionTest, IndexScanNodeDoesNotRemoveCollatedFieldsFromSortsIfCollationMatches) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    node.computeProperties();

    auto sorts = node.providedSorts();
    ASSERT_EQ(sorts, ProvidedSortSet(BSON("a" << 1), {}));

    ASSERT_TRUE(sorts.contains(BSON("a" << 1)));
}

// Index: {a: 1, b: "hashed", c: 1}
// Bounds: a: [1, 1], b: [MINKEY, MAXKEY], c: [1, 2]
TEST(QuerySolutionTest, HashedIndexScanNodeTruncatesSort) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b"
                                                      << "hashed"
                                                      << "c" << 1))};
    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    node.computeProperties();
    auto sorts = node.providedSorts();

    // The hashed field cannot be part of the provided sort and forces the rest of the sort to be
    // truncated.
    ASSERT_EQ(sorts, ProvidedSortSet(BSONObj(), {"a"} /* equality fields */));
    ASSERT(sorts.contains(BSON("a" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1 << "c" << 1)));
}

// Index: {a: 1, b: "hashed", c: 1}
// Bounds: a: [1, 1], b: [MINKEY, MAXKEY], c: [1, 1]
TEST(QuerySolutionTest, HashedIndexScanNodeTruncatesSortUnlessFollowedByEquality) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b"
                                                      << "hashed"
                                                      << "c" << 1))};
    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(c);

    node.computeProperties();
    auto sorts = node.providedSorts();

    // The hashed field cannot be part of the provided sort and forces the rest of the sort to be
    // truncated except for following equalities.
    ASSERT_EQ(sorts, ProvidedSortSet(BSONObj(), {"a", "c"} /* equality fields */));
    ASSERT(sorts.contains(BSON("a" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1)));
    ASSERT(sorts.contains(BSON("c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b" << 1)));
    ASSERT(sorts.contains(BSON("a" << 1 << "c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1 << "c" << 1)));
}

// Index: {a: 1, b: "hashed", c: 1}
// Bounds: a: [1, 1], b: [1, 1], c: [MINKEY, MAXKEY]
TEST(QuerySolutionTest, HashedIndexScanNodeDoesNotTruncateSortWhenEquality) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b"
                                                      << "hashed"
                                                      << "c" << 1))};
    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    node.computeProperties();
    auto sorts = node.providedSorts();

    // The hashed field can be part of the sort when its bounds are a point interval (equality). It
    // does not cause truncation in this case.
    ASSERT_EQ(sorts, ProvidedSortSet(BSON("c" << 1), {"a", "b"} /* equality fields */));
    ASSERT(sorts.contains(BSON("a" << 1)));
    ASSERT(sorts.contains(BSON("b" << 1)));
    ASSERT(sorts.contains(BSON("c" << 1)));
    ASSERT(sorts.contains(BSON("a" << 1 << "b" << 1)));
    ASSERT(sorts.contains(BSON("a" << 1 << "c" << 1)));
    ASSERT(sorts.contains(BSON("b" << 1 << "c" << 1)));
}

// Index: {a: 1, b: "hashed", c: 1}
// Bounds: a: [1, 1], b: ["p", "p"], c: [MINKEY, MAXKEY]
TEST(QuerySolutionTest, HashedIndexScanNodeDoesTruncatesSortWhenCollationDoesntMatch) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b"
                                                      << "hashed"
                                                      << "c" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kToLowerString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << "p")));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    node.computeProperties();
    auto sorts = node.providedSorts();

    // The hashed field cannot be part of the sort when its bounds are a point interval (equality)
    // that is affected by collation and the query and index collation do not match. It forces the
    // rest of the sort to be truncated.
    ASSERT_EQ(sorts, ProvidedSortSet(BSONObj(), {"a"} /* equality fields */));
    ASSERT(sorts.contains(BSON("a" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1 << "c" << 1)));
}

// Index: {a: 1, b: "hashed", c: 1}
// Bounds: a: [1, 1], b: ["a", "b"], c: [MINKEY, MAXKEY]
TEST(QuerySolutionTest,
     HashedIndexScanNodeDoesTruncatesSortWhenCollationDoesntMatchWithRangeQuery) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b"
                                                      << "hashed"
                                                      << "c" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kToLowerString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << "a"
                                                      << ""
                                                      << "b"),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    node.computeProperties();
    auto sorts = node.providedSorts();

    // The hashed field cannot be part of the sort when its bounds are a range interval that is
    // affected by collation and the query and index collation do not match. It forces the rest of
    // the sort to be truncated.
    ASSERT_EQ(sorts, ProvidedSortSet(BSONObj(), {"a"} /* equality fields */));
    ASSERT(sorts.contains(BSON("a" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1 << "c" << 1)));
}

// Expanded wildcard index: {a: 1, b.$**: 1, c: 1}
// Bounds: a: [1, 1], $_path: ["b", "b"], b: [1, 2], c: [2, 3]
TEST(QuerySolutionTest, WildcardIndexSupportsSortWhenIndexOnlyNeedsToLookAtOnePath) {
    mongo::wildcard_planning::WildcardIndexEntryMock wildcardIndex{
        BSON("a" << 1 << "b.$**" << 1 << "c" << 1), BSONObj{}, {}};

    // The following setup mimics a query that queries against fields "a", "b.d", and "c". This
    // matches the bounds we generate for those fields below. However, only pass "b.d" as 'fields'
    // here, because we only want to consider the expanded index with that field plugged in.
    std::set<std::string> fields{"b.d"};
    std::vector<IndexEntry> expandedIndexes{};
    mongo::wildcard_planning::expandWildcardIndexEntry(
        *wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(expandedIndexes.size(), 1);
    IndexScanNode node{expandedIndexes.at(0)};

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b.d";
    b.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));

    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 2 << "" << 3), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    std::vector<interval_evaluation_tree::Builder> ietBuilders;
    mongo::wildcard_planning::finalizeWildcardIndexScanConfiguration(&node, &ietBuilders);

    node.computeProperties();
    auto sorts = node.providedSorts();

    // When the wildcard path is only on one point, it can provide a sort on the wildcard field
    // (b.d). "$_path" is a reserved key used for describing the wildcard path. It appears here in
    // fields with equality bounds.
    ASSERT_EQ(sorts,
              ProvidedSortSet(BSON("b.d" << 1 << "c" << 1), {"a", "$_path"} /* equality fields */));
    ASSERT(sorts.contains(BSON("a" << 1)));
    ASSERT(sorts.contains(BSON("b.d" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("c" << 1)));
    ASSERT(sorts.contains(BSON("a" << 1 << "b.d" << 1)));
    ASSERT(sorts.contains(BSON("b.d" << 1 << "c" << 1)));
    ASSERT(sorts.contains(BSON("a" << 1 << "b.d" << 1 << "c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "c" << 1)));
}

// Index: {a: 1, b.$**: 1, c: 1}
// Bounds: a: [1, 1], b: [MINKEY, MAXKEY], c: [2, 3]
TEST(QuerySolutionTest, WildcardIndexDoesNotSupportSortWhenIndexNeedsToLookAtMultiplePaths) {
    // The following setup mimics a query that queries against fields "a", "b", and "c". This
    // matches the bounds we generate for those fields below. However, only pass "b" as 'fields'
    // here, because we only want to consider the expanded index with that field plugged in.
    mongo::wildcard_planning::WildcardIndexEntryMock wildcardIndex{
        BSON("a" << 1 << "b.$**" << 1 << "c" << 1), BSONObj{}, {}};
    std::set<std::string> fields{"b"};
    std::vector<IndexEntry> expandedIndexes{};
    mongo::wildcard_planning::expandWildcardIndexEntry(
        *wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(expandedIndexes.size(), 1);
    IndexScanNode node{expandedIndexes.at(0)};

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 2 << "" << 3), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    std::vector<interval_evaluation_tree::Builder> ietBuilders;
    mongo::wildcard_planning::finalizeWildcardIndexScanConfiguration(&node, &ietBuilders);

    node.computeProperties();
    auto sorts = node.providedSorts();

    // When the wildcard path is a union, it cannot provide a sort on any wildcard field.
    ASSERT_EQ(sorts, ProvidedSortSet(BSONObj(), {"a"} /* equality fields */));
    ASSERT(sorts.contains(BSON("a" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b.d" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "c" << 1)));
}

// Index: {a: 1, b.$**: 1, c: 1}
// Bounds: a: [1, 1], b: ["p", "p"], c: [2, 3]
TEST(QuerySolutionTest, WildcardIndexDoesNotSupportSortWhenCollationDoesntMatch) {
    // The following setup mimics a query that queries against fields "a", "b.d", and "c". This
    // matches the bounds we generate for those fields below.  However, only pass "b.d" as 'fields'
    // here, because we only want to consider the expanded index with that field plugged in.
    mongo::wildcard_planning::WildcardIndexEntryMock wildcardIndex{
        BSON("a" << 1 << "b.$**" << 1 << "c" << 1), BSONObj{}, {}};
    std::set<std::string> fields{"b.d"};
    std::vector<IndexEntry> expandedIndexes{};
    mongo::wildcard_planning::expandWildcardIndexEntry(
        *wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(expandedIndexes.size(), 1);
    IndexScanNode node{expandedIndexes.at(0)};

    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kToLowerString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b.d";
    b.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << "p")));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 2 << "" << 3), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);


    std::vector<interval_evaluation_tree::Builder> ietBuilders;
    mongo::wildcard_planning::finalizeWildcardIndexScanConfiguration(&node, &ietBuilders);

    node.computeProperties();

    // When the wildcard path is only on one point, but the wildcard field matches string values
    // and the query and indxex collation differ, then the index cannot provide a sort on the
    // wildcard field (b.d). It does not cause truncation because the bounds for b.d are a point.
    auto sorts = node.providedSorts();
    ASSERT_EQ(sorts, ProvidedSortSet(BSON("c" << 1), {"a"} /* equality fields */));
    ASSERT(sorts.contains(BSON("a" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b.d" << 1)));
    ASSERT(sorts.contains(BSON("c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b.d" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b.d" << 1 << "c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b.d" << 1 << "c" << 1)));
    ASSERT(sorts.contains(BSON("a" << 1 << "c" << 1)));
}

// Index: {a: 1, b.$**: 1, c: 1}
// Bounds: a: [1, 1], b: ["a", "b"], c: [2, 3]
TEST(QuerySolutionTest, WildcardIndexDoesNotSupportSortWhenCollationDoesntMatchWithRangeQuery) {
    // The following setup mimics a query that queries against fields "a", "b.d", and "c". This
    // matches the bounds we generate for those fields below. However, only pass "b.d" as 'fields'
    // here, because we only want to consider the expanded index with that field plugged in.
    mongo::wildcard_planning::WildcardIndexEntryMock wildcardIndex{
        BSON("a" << 1 << "b.$**" << 1 << "c" << 1), BSONObj{}, {}};
    std::set<std::string> fields{"b.d"};
    std::vector<IndexEntry> expandedIndexes{};
    mongo::wildcard_planning::expandWildcardIndexEntry(
        *wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(expandedIndexes.size(), 1);
    IndexScanNode node{expandedIndexes.at(0)};

    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kToLowerString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b.d";
    b.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << "a"
                                                      << ""
                                                      << "b"),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 2 << "" << 3), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    std::vector<interval_evaluation_tree::Builder> ietBuilders;
    mongo::wildcard_planning::finalizeWildcardIndexScanConfiguration(&node, &ietBuilders);

    node.computeProperties();
    auto sorts = node.providedSorts();

    // When the wildcard path is only on one point, but the wildcard field matches string values
    // and the query and indxex collation differ, then the index cannot provide a sort on the
    // wildcard field (b.d). It also causes truncation because the bounds for b.d are a range.
    ASSERT_EQ(sorts, ProvidedSortSet(BSONObj(), {"a"} /* equality fields */));
    ASSERT(sorts.contains(BSON("a" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b.d" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b.d" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("b.d" << 1 << "c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "b.d" << 1 << "c" << 1)));
    ASSERT_FALSE(sorts.contains(BSON("a" << 1 << "c" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Intervals: a: [1,1], b: ['p','p'], c: [1,2], d: [MinKey, MaxKey], e: [1,2]
TEST(QuerySolutionTest, CompoundIndexWithNonMatchingCollationFiltersAllSortsWithCollatedField) {
    IndexScanNode node{
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    node.index =
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1));

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << "p")));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    OrderedIntervalList d{};
    d.name = "d";
    d.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(d);

    OrderedIntervalList e{};
    e.name = "e";
    e.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(e);

    node.computeProperties();

    // Expected sort orders
    ASSERT_EQ(node.providedSorts(), ProvidedSortSet(BSON("c" << 1), {"a"}));

    ASSERT(node.providedSorts().contains(BSON("a" << 1)));
    ASSERT_FALSE(node.providedSorts().contains(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "c" << 1)));
    ASSERT_FALSE(node.providedSorts().contains(BSON("a" << 1 << "c" << 1 << "d" << 1)));
    ASSERT_FALSE(node.providedSorts().contains(BSON("a" << 1 << "c" << 1 << "e" << 1)));
}

// Index: {a : 1}
// Bounds: [{}, {}]
TEST(QuerySolutionTest, IndexScanNodeWithNonMatchingCollationFiltersObjectField) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << BSON("foo" << 1) << "" << BSON("foo" << 2)),
        BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    node.computeProperties();

    ASSERT_EQ(node.providedSorts(), kEmptySet);
}

// Index: {a : 1}
// Bounds: [[], []]
TEST(QuerySolutionTest, IndexScanNodeWithNonMatchingCollationFiltersArrayField) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << BSON_ARRAY(1) << "" << BSON_ARRAY(2)),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    node.computeProperties();

    ASSERT_EQ(node.providedSorts(), kEmptySet);
}

TEST(QuerySolutionTest, WithNonMatchingCollatorAndNoEqualityPrefixSortsAreNotDuplicated) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    OrderedIntervalList oilB{};
    oilB.name = "b";
    oilB.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << "a"
                                                      << ""
                                                      << "b"),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilB);
    node.computeProperties();

    // Expected sort orders
    ASSERT_EQ(node.providedSorts(), ProvidedSortSet(BSON("a" << 1), {}));
    ASSERT(node.providedSorts().contains(BSON("a" << 1)));
}

TEST(QuerySolutionTest, IndexScanNodeHasFieldIncludesStringFieldWhenNoCollator) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1))};

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << "str"
                                                      << ""
                                                      << "str"),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    OrderedIntervalList oilB{};
    oilB.name = "b";
    oilB.intervals.push_back(IndexBoundsBuilder::allValues());
    node.bounds.fields.push_back(oilB);

    ASSERT_TRUE(node.hasField("a"));
    ASSERT_TRUE(node.hasField("b"));
}

TEST(QuerySolutionTest, IndexScanNodeHasFieldIncludesSimpleBoundsStringFieldWhenNoCollator) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1))};

    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 2);
    node.bounds.endKey = BSON("a" << 2 << "b" << 1);
    node.bounds.boundInclusion = BoundInclusion::kIncludeStartKeyOnly;

    ASSERT_TRUE(node.hasField("a"));
    ASSERT_TRUE(node.hasField("b"));
}

TEST(QuerySolutionTest, IndexScanNodeHasFieldExcludesStringFieldWhenIndexHasCollator) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.index.collator = &indexCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    OrderedIntervalList oilB{};
    oilB.name = "b";
    oilB.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << "bar"
                                                      << ""
                                                      << "foo"),
                                              BoundInclusion::kIncludeStartKeyOnly));
    node.bounds.fields.push_back(oilB);

    ASSERT_TRUE(node.hasField("a"));
    ASSERT_FALSE(node.hasField("b"));
}

TEST(QuerySolutionTest, IndexScanNodeHasFieldExcludesSimpleBoundsStringFieldWhenIndexHasCollator) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.index.collator = &indexCollator;

    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 2);
    node.bounds.endKey = BSON("a" << 2 << "b" << 1);
    node.bounds.boundInclusion = BoundInclusion::kIncludeStartKeyOnly;

    ASSERT_TRUE(node.hasField("a"));
    ASSERT_FALSE(node.hasField("b"));
}

auto createMatchExprAndProjection(const BSONObj& query, const BSONObj& projObj) {
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    const auto expCtx = ExpressionContextBuilder{}
                            .opCtx(opCtx.get())
                            .ns(NamespaceString::createNamespaceString_forTest("test.dummy"))
                            .build();
    StatusWithMatchExpression queryMatchExpr = MatchExpressionParser::parse(query, expCtx);
    ASSERT(queryMatchExpr.isOK());
    projection_ast::Projection res = projection_ast::parseAndAnalyze(
        expCtx, projObj, queryMatchExpr.getValue().get(), query, ProjectionPolicies{});
    return std::make_pair(std::move(queryMatchExpr.getValue()), std::move(res));
}

TEST(QuerySolutionTest, InclusionProjectionPreservesSort) {
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    auto node = std::make_unique<IndexScanNode>(index);

    BSONObj projection = BSON("a" << 1);
    BSONObj match;

    auto matchExprAndProjection = createMatchExprAndProjection(match, projection);

    ProjectionNodeDefault proj{
        std::move(node), matchExprAndProjection.first.get(), matchExprAndProjection.second};
    proj.computeProperties();

    ASSERT_EQ(proj.providedSorts(), ProvidedSortSet(BSON("a" << 1), {}));
    ASSERT(proj.providedSorts().contains(BSON("a" << 1)));
}

TEST(QuerySolutionTest, ExclusionProjectionDoesNotPreserveSort) {
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    auto node = std::make_unique<IndexScanNode>(index);

    BSONObj projection = BSON("a" << 0);
    BSONObj match;

    auto matchExprAndProjection = createMatchExprAndProjection(match, projection);

    ProjectionNodeDefault proj{
        std::move(node), matchExprAndProjection.first.get(), matchExprAndProjection.second};
    proj.computeProperties();

    ASSERT_EQ(proj.providedSorts(), kEmptySet);
    ASSERT(!proj.providedSorts().contains(BSON("a" << 1)));
}

TEST(QuerySolutionTest, InclusionProjectionTruncatesSort) {
    auto node = std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1)));

    BSONObj projection = BSON("a" << 1);
    BSONObj match;

    auto matchExprAndProjection = createMatchExprAndProjection(match, projection);

    ProjectionNodeDefault proj{
        std::move(node), matchExprAndProjection.first.get(), matchExprAndProjection.second};
    proj.computeProperties();

    ASSERT_EQ(proj.providedSorts(), ProvidedSortSet(BSON("a" << 1), {}));
    ASSERT(proj.providedSorts().contains(BSON("a" << 1)));
}

TEST(QuerySolutionTest, ExclusionProjectionTruncatesSort) {
    auto node = std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1)));

    BSONObj projection = BSON("b" << 0);
    BSONObj match;

    auto matchExprAndProjection = createMatchExprAndProjection(match, projection);

    ProjectionNodeDefault proj{
        std::move(node), matchExprAndProjection.first.get(), matchExprAndProjection.second};
    proj.computeProperties();

    ASSERT_EQ(proj.providedSorts(), ProvidedSortSet(BSON("a" << 1), {}));
    ASSERT(proj.providedSorts().contains(BSON("a" << 1)));
    ASSERT_FALSE(proj.providedSorts().contains(BSON("a" << 1 << "b" << 1)));
}

TEST(QuerySolutionTest, NonMultikeyIndexWithoutPathLevelInfoCanCoverItsFields) {
    auto node =
        std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1 << "b.c.d" << 1)));
    node->index.multikey = false;
    node->index.multikeyPaths = MultikeyPaths{};
    ASSERT_TRUE(node->hasField("a"));
    ASSERT_TRUE(node->hasField("b.c.d"));
    ASSERT_FALSE(node->hasField("b.c"));
    ASSERT_FALSE(node->hasField("b"));
    ASSERT_FALSE(node->hasField("e"));
}

TEST(QuerySolutionTest, NonMultikeyIndexWithPathLevelInfoCanCoverItsFields) {
    auto node =
        std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1 << "b.c.d" << 1)));
    node->index.multikey = false;
    node->index.multikeyPaths = MultikeyPaths{{}, {}};
    ASSERT_TRUE(node->hasField("a"));
    ASSERT_TRUE(node->hasField("b.c.d"));
    ASSERT_FALSE(node->hasField("b.c"));
    ASSERT_FALSE(node->hasField("b"));
    ASSERT_FALSE(node->hasField("e"));
}

TEST(QuerySolutionTest, MultikeyIndexWithoutPathLevelInfoCannotCoverAnyFields) {
    auto node =
        std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1 << "b.c.d" << 1)));
    node->index.multikey = true;
    node->index.multikeyPaths = MultikeyPaths{};
    ASSERT_FALSE(node->hasField("a"));
    ASSERT_FALSE(node->hasField("b.c.d"));
    ASSERT_FALSE(node->hasField("b.c"));
    ASSERT_FALSE(node->hasField("b"));
    ASSERT_FALSE(node->hasField("e"));
}

TEST(QuerySolutionTest, MultikeyIndexWithPathLevelInfoCanCoverNonMultikeyFields) {
    auto node = std::make_unique<IndexScanNode>(
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1)));

    // Add metadata indicating that "b" is multikey.
    node->index.multikey = true;
    node->index.multikeyPaths = MultikeyPaths{{}, {0U}, {}};

    ASSERT_TRUE(node->hasField("a"));
    ASSERT_FALSE(node->hasField("b"));
    ASSERT_FALSE(node->hasField("b.c"));
    ASSERT_TRUE(node->hasField("c"));
}

TEST(QuerySolutionTest, MultikeyIndexCannotCoverFieldWithAnyMultikeyPathComponent) {
    auto node = std::make_unique<IndexScanNode>(
        buildSimpleIndexEntry(BSON("a" << 1 << "b.c.d" << 1 << "e" << 1)));

    // Add metadata indicating that "b.c" is multikey.
    node->index.multikey = true;
    node->index.multikeyPaths = MultikeyPaths{{}, {1U}, {}};

    ASSERT_TRUE(node->hasField("a"));
    ASSERT_FALSE(node->hasField("b"));
    ASSERT_FALSE(node->hasField("b.c"));
    ASSERT_FALSE(node->hasField("b.c.d"));
    ASSERT_TRUE(node->hasField("e"));
}

TEST(QuerySolutionTest, MultikeyIndexWithoutPathLevelInfoCannotProvideAnySorts) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1))};
    node.index.multikey = true;

    {
        OrderedIntervalList oil{};
        oil.name = "a";
        oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
        node.bounds.fields.push_back(oil);
    }

    for (auto&& name : {"b"_sd, "c"_sd}) {
        OrderedIntervalList oil{};
        oil.name = std::string{name};
        oil.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
            BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
        node.bounds.fields.push_back(oil);
    }

    node.computeProperties();
    ASSERT_EQ(node.providedSorts(), kEmptySet);
}

// Index: {a: 1, b: 1, 'c.z': 1, d: 1, e: 1}
// Intervals: a: [1,1], b: [1,1], 'c.z': [1,1], d: [1, 1], e: [1,2]
// Multikeys: ['b', 'c.z']
TEST(QuerySolutionTest, SimpleRangeWithEqualIgnoresFieldWithMultikeyComponent) {
    IndexScanNode node{
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c.z" << 1 << "d" << 1 << "e" << 1))};
    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1 << "c.z" << 1 << "d" << 1 << "e" << 1);
    node.bounds.endKey = BSON("a" << 1 << "b" << 1 << "c.z" << 1 << "d" << 1 << "e" << 2);

    // Add metadata indicating that 'b', 'c.z' is multikey.
    node.index.multikey = true;
    node.index.multikeyPaths = MultikeyPaths{{}, {1U}, {1U}, {}, {}};

    node.computeProperties();

    ASSERT_EQ(node.providedSorts(), ProvidedSortSet(BSON("e" << 1), {"a", "d"}));

    ASSERT(node.providedSorts().contains(BSON("a" << 1)));
    ASSERT(node.providedSorts().contains(BSON("d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "d" << -1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("e" << 1)));

    // Cannot provide sorts that include a multikey field.
    ASSERT_FALSE(node.providedSorts().contains(BSON("a" << 1 << "b" << -1)));
    ASSERT_FALSE(node.providedSorts().contains(BSON("a" << 1 << "c.z" << 1)));
    ASSERT_FALSE(node.providedSorts().contains(BSON("c.z" << 1)));
}

TEST(QuerySolutionTest, MultikeyFieldsEmptyWhenIndexIsNotMultikey) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a.b" << 1 << "c.d" << 1))};
    node.index.multikey = false;
    node.index.multikeyPaths = MultikeyPaths{};
    node.computeProperties();
    ASSERT(node.multikeyFields.empty());
}

TEST(QuerySolutionTest, MultikeyFieldsEmptyWhenIndexHasNoMultikeynessMetadata) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a.b" << 1 << "c.d" << 1))};
    node.index.multikey = true;
    node.index.multikeyPaths = MultikeyPaths{};
    node.computeProperties();
    ASSERT(node.multikeyFields.empty());
}

TEST(QuerySolutionTest, MultikeyFieldsChosenCorrectlyWhenIndexHasPathLevelMultikeyMetadata) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("a.b" << 1 << "c.d" << 1 << "e.f" << 1))};
    node.index.multikey = true;
    node.index.multikeyPaths = MultikeyPaths{{0U}, {}, {0U, 1U}};
    node.computeProperties();
    ASSERT_EQ(node.multikeyFields.size(), 2U);
    ASSERT(node.multikeyFields.count("a.b"));
    ASSERT(node.multikeyFields.count("e.f"));
}

TEST(QuerySolutionTest, NonSimpleRangeAllEqualExcludesFieldWithMultikeyComponent) {
    IndexScanNode node{
        buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c.z" << 1 << "d" << 1 << "e" << 1))};
    // Add metadata indicating that "c.z" is multikey.
    node.index.multikey = true;
    node.index.multikeyPaths = MultikeyPaths{{}, {}, {1U}, {}, {}};

    for (auto&& name : {"a"_sd, "b"_sd, "c.z"_sd, "d"_sd, "e"_sd}) {
        OrderedIntervalList oil{};
        oil.name = std::string{name};
        oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
        node.bounds.fields.push_back(oil);
    }

    node.computeProperties();
    ASSERT_EQ(node.providedSorts(), ProvidedSortSet(BSONObj(), {"a", "b", "d", "e"}));

    ASSERT(node.providedSorts().contains(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.providedSorts().contains(BSON("a" << 1)));
    ASSERT(node.providedSorts().contains(BSON("d" << 1 << "e" << 1)));
    ASSERT(node.providedSorts().contains(BSON("e" << 1)));
    ASSERT_FALSE(node.providedSorts().contains(BSON("a" << 1 << "b" << 1 << "c.z" << 1)));
}

TEST(QuerySolutionTest, SharedPrefixMultikeyNonMinMaxBoundsDoesNotProvideAnySorts) {
    IndexScanNode node{buildSimpleIndexEntry(BSON("c.x" << 1 << "c.z" << 1))};

    node.index.multikey = true;
    node.index.multikeyPaths = MultikeyPaths{{1U}, {1U}};

    {
        OrderedIntervalList oil{};
        oil.name = "c.x";
        oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
        node.bounds.fields.push_back(oil);
    }
    {
        OrderedIntervalList oil{};
        oil.name = "c.z";
        oil.intervals.push_back(IndexBoundsBuilder::allValues());
        node.bounds.fields.push_back(oil);
    }

    node.computeProperties();
    ASSERT_EQ(node.providedSorts(), kEmptySet);
}

TEST(QuerySolutionTest, NodeIdsAssignedInPostOrderFashionStartingFromOne) {
    // Construct a QuerySolution consisting of a root node with two children.
    std::vector<std::unique_ptr<QuerySolutionNode>> children;
    children.push_back(std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1))));
    children.push_back(std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("b" << 1))));
    auto orNode = std::make_unique<OrNode>();
    orNode->addChildren(std::move(children));

    // Before being added to the QuerySolution, all the nodes should have a nodeId of zero, which
    // means that an id has not yet been assigned.
    ASSERT_EQ(orNode->nodeId(), 0u);
    ASSERT_EQ(orNode->children[0]->nodeId(), 0u);
    ASSERT_EQ(orNode->children[1]->nodeId(), 0u);

    auto querySolution = std::make_unique<QuerySolution>();
    querySolution->setRoot(std::move(orNode));
    auto root = querySolution->root();

    // Since ids are assigned according to a post-order traversal, the root node should have id 3,
    // the left child should have id 1, and the right child should have id 2.
    ASSERT_EQ(root->nodeId(), 3);
    ASSERT_EQ(root->children[0]->nodeId(), 1);
    ASSERT_EQ(root->children[1]->nodeId(), 2);
}

TEST(QuerySolutionTest, GroupNodeWithIndexScan) {
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    const auto expCtx = ExpressionContextBuilder{}
                            .opCtx(opCtx.get())
                            .ns(NamespaceString::createNamespaceString_forTest("test.dummy"))
                            .build();
    auto scanNode =
        std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1)));
    scanNode->bounds.isSimpleRange = true;
    scanNode->bounds.startKey = BSON("a" << 1 << "b" << 1);
    scanNode->bounds.endKey = BSON("a" << 1 << "b" << 1);
    GroupNode node(
        std::move(scanNode),
        boost::intrusive_ptr<ExpressionConstant>(new ExpressionConstant(expCtx.get(), Value(0))),
        {},
        false,
        false,
        false);
    node.computeProperties();

    ASSERT_EQ(node.fetched(), true);
    ASSERT_EQ(node.sortedByDiskLoc(), false);
    ASSERT_EQ(node.providedSorts(), kEmptySet);

    ASSERT_EQ(node.getFieldAvailability("any_field"), FieldAvailability::kNotProvided);
}

TEST(QuerySolutionTest, EqLookupNodeWithIndexScan) {
    // Simple EqLookupNode with IndexScan subtree.
    auto scanNode =
        std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1)));
    scanNode->bounds.isSimpleRange = true;
    scanNode->bounds.startKey = BSON("a" << 1 << "b" << 1);
    scanNode->bounds.endKey = BSON("a" << 1 << "b" << 1);

    EqLookupNode node(std::move(scanNode),
                      NamespaceString::createNamespaceString_forTest("db.col"),
                      "local",
                      "foreign",
                      "as",
                      EqLookupNode::LookupStrategy::kNestedLoopJoin,
                      boost::none /* idxEntry */,
                      false /* shouldProduceBson */);

    node.computeProperties();

    auto child = node.children[0].get();
    ASSERT_EQ(node.fetched(), child->fetched());
    ASSERT_EQ(node.sortedByDiskLoc(), child->sortedByDiskLoc());

    // Expected empty sort order, as the EqLookupNode order inferrence is not supported yet.
    ASSERT_EQ(node.providedSorts(), kEmptySet);

    ASSERT_EQ(node.getFieldAvailability("as"), FieldAvailability::kNotProvided);
    ASSERT_EQ(node.getFieldAvailability("a"), child->getFieldAvailability("a"));
    ASSERT_EQ(node.getFieldAvailability("b"), child->getFieldAvailability("b"));
}

TEST(QuerySolutionTest, EqLookupNodeWithIndexScanFieldOverwrite) {
    // A EqLookupNode with IndexScan subtree, where local field "b" is overwritten.
    // This affects the field availability and sort order.
    auto scanNode =
        std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1 << "b" << 1 << "c"
                                                                       << "1")));
    scanNode->bounds.isSimpleRange = true;
    scanNode->bounds.startKey = BSON("a" << 1 << "b" << 1 << "c"
                                         << "1");
    scanNode->bounds.endKey = BSON("a" << 1 << "b" << 1 << "c"
                                       << "1");

    EqLookupNode node(std::move(scanNode),
                      NamespaceString::createNamespaceString_forTest("db.col"),
                      "local",
                      "foreign",
                      "b",
                      EqLookupNode::LookupStrategy::kNestedLoopJoin,
                      boost::none /* idxEntry */,
                      false /* shouldProduceBson */);

    node.computeProperties();

    auto child = node.children[0].get();
    // Expected empty sort order, as the EqLookupNode order inferrence is not supported yet.
    ASSERT_EQ(node.providedSorts(), kEmptySet);

    ASSERT_EQ(node.getFieldAvailability("a"), child->getFieldAvailability("a"));
    ASSERT_EQ(node.getFieldAvailability("b"), FieldAvailability::kNotProvided);
    ASSERT_EQ(node.getFieldAvailability("c"), child->getFieldAvailability("c"));
    ASSERT_EQ(node.getFieldAvailability("other"), child->getFieldAvailability("other"));
}

TEST(QuerySolutionTest, ProvidedSortSetOutputStreamOperator) {
    ProvidedSortSet ex1;
    std::stringstream ex1_stream;
    ex1_stream << ex1;
    ASSERT_EQ(ex1_stream.str(), "baseSortPattern: {}, ignoredFields: []");

    ProvidedSortSet ex2(BSONObj(), {"b", "a"});
    std::stringstream ex2_stream;
    ex2_stream << ex2;
    ASSERT_EQ(ex2_stream.str(), "baseSortPattern: {}, ignoredFields: [a, b]");

    ProvidedSortSet ex3(BSON("b" << 1 << "a" << 1), {});
    std::stringstream ex3_stream;
    ex3_stream << ex3;
    ASSERT_EQ(ex3_stream.str(), "baseSortPattern: { b: 1, a: 1 }, ignoredFields: []");

    ProvidedSortSet ex4(BSON("a" << 1 << "b" << 1), {"c", "d"});
    std::stringstream ex4_stream;
    ex4_stream << ex4;
    ASSERT_EQ(ex4_stream.str(), "baseSortPattern: { a: 1, b: 1 }, ignoredFields: [c, d]");
}

TEST(QuerySolutionTest, ProvidedSortSetEqualityOperators) {
    ProvidedSortSet ex1(BSON("a" << 1 << "b" << 1), {"c", "d"});
    ProvidedSortSet ex2(BSON("b" << 1 << "a" << 1), {"c", "d"});
    ProvidedSortSet ex3(BSON("a" << 1 << "b" << 1), {"d", "c"});
    ProvidedSortSet ex4(BSON("a" << 1 << "b" << 1), {"c", "d", "e"});
    ProvidedSortSet ex5(BSON("a" << 1 << "b" << 1 << "c" << 1), {"d", "c"});

    ASSERT_EQ(ex1, ex1);
    ASSERT_NE(ex1, ex2);
    ASSERT_EQ(ex1, ex3);
    ASSERT_NE(ex1, ex4);
    ASSERT_NE(ex1, ex5);
}

TEST(QuerySolutionTest, FieldAvailabilityOutputStreamOperator) {
    std::stringstream ex1;
    ex1 << FieldAvailability::kNotProvided;
    ASSERT_EQ(ex1.str(), "NotProvided");

    std::stringstream ex2;
    ex2 << FieldAvailability::kFullyProvided;
    ASSERT_EQ(ex2.str(), "FullyProvided");

    std::stringstream ex3;
    ex3 << FieldAvailability::kHashedValueProvided;
    ASSERT_EQ(ex3.str(), "HashedValueProvided");
}

TEST(QuerySolutionTest, GetSecondaryNamespaceVectorOverSingleEqLookupNode) {
    auto scanNode = std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1)));
    const NamespaceString mainNss = NamespaceString::createNamespaceString_forTest("db.main");
    const NamespaceString foreignColl = NamespaceString::createNamespaceString_forTest("db.col");
    auto root = std::make_unique<EqLookupNode>(std::move(scanNode),
                                               foreignColl,
                                               "local",
                                               "remote",
                                               "b",
                                               EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                               boost::none /* idxEntry */,
                                               false /* shouldProduceBson */);


    QuerySolution qs;
    qs.setRoot(std::move(root));

    // The output vector should only contain 'foreignColl'.
    std::vector<NamespaceStringOrUUID> expectedNssVector{foreignColl};
    assertNamespaceVectorsAreEqual(qs.getAllSecondaryNamespaces(mainNss), expectedNssVector);
}

TEST(QuerySolutionTest, AssertSameHashes) {
    auto makeQs = []() {
        auto scanNode = std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1)));
        const NamespaceString mainNss = NamespaceString::createNamespaceString_forTest("db.main");
        const NamespaceString foreignColl =
            NamespaceString::createNamespaceString_forTest("db.col");
        auto root = std::make_unique<EqLookupNode>(std::move(scanNode),
                                                   foreignColl,
                                                   "local",
                                                   "remote",
                                                   "b",
                                                   EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                                   boost::none /* idxEntry */,
                                                   false /* shouldProduceBson */);


        auto qs = std::make_unique<QuerySolution>();
        qs->setRoot(std::move(root));
        return qs;
    };
    ASSERT(makeQs()->hash() == makeQs()->hash());
}

TEST(QuerySolutionTest, GetSecondaryNamespaceVectorDeduplicatesMainNss) {
    auto scanNode = std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1)));
    const NamespaceString mainNss = NamespaceString::createNamespaceString_forTest("db.main");
    auto root = std::make_unique<EqLookupNode>(std::move(scanNode),
                                               mainNss,
                                               "local",
                                               "remote",
                                               "b",
                                               EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                               boost::none /* idxEntry */,
                                               false /* shouldProduceBson */);


    QuerySolution qs;
    qs.setRoot(std::move(root));

    // There should be no secondary namespaces as 'mainNss' is ignored in
    // 'getAllSecondaryNamespaces'.
    std::vector<NamespaceStringOrUUID> expectedNssVector{};
    assertNamespaceVectorsAreEqual(qs.getAllSecondaryNamespaces(mainNss), expectedNssVector);
}

TEST(QuerySolutionTest, GetSecondaryNamespaceVectorOverNestedEqLookupNodes) {
    auto scanNode = std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1)));
    const NamespaceString mainNss = NamespaceString::createNamespaceString_forTest("db.main");
    const NamespaceString foreignCollOne = NamespaceString::createNamespaceString_forTest("db.col");
    const NamespaceString foreignCollTwo = NamespaceString::createNamespaceString_forTest("db.foo");
    auto childEqLookupNode =
        std::make_unique<EqLookupNode>(std::move(scanNode),
                                       foreignCollOne,
                                       "local",
                                       "remote",
                                       "b",
                                       EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                       boost::none /* idxEntry */,
                                       false /* shouldProduceBson */);

    auto parentEqLookupNode =
        std::make_unique<EqLookupNode>(std::move(childEqLookupNode),
                                       foreignCollTwo,
                                       "local",
                                       "remote",
                                       "b",
                                       EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                       boost::none /* idxEntry */,
                                       false /* shouldProduceBson */);

    QuerySolution qs;
    qs.setRoot(std::move(parentEqLookupNode));

    // The foreign collections are unique, so our output vector should contain both of them. Note
    // that because 'getAllSecondaryNamespaces' uses a set internally, these namespaces are
    // expected to be in sorted order in the output vector.
    std::vector<NamespaceStringOrUUID> expectedNssVector{foreignCollOne, foreignCollTwo};
    assertNamespaceVectorsAreEqual(qs.getAllSecondaryNamespaces(mainNss), expectedNssVector);
}

TEST(QuerySolutionTest, GetSecondaryNamespaceVectorDeduplicatesNestedEqLookupNodes) {
    auto scanNode = std::make_unique<IndexScanNode>(buildSimpleIndexEntry(BSON("a" << 1)));
    const NamespaceString mainNss = NamespaceString::createNamespaceString_forTest("db.main");
    const NamespaceString foreignColl = NamespaceString::createNamespaceString_forTest("db.col");
    auto childEqLookupNode =
        std::make_unique<EqLookupNode>(std::move(scanNode),
                                       foreignColl,
                                       "local",
                                       "remote",
                                       "b",
                                       EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                       boost::none /* idxEntry */,
                                       false /* shouldProduceBson */);

    auto parentEqLookupNode =
        std::make_unique<EqLookupNode>(std::move(childEqLookupNode),
                                       foreignColl,
                                       "local",
                                       "remote",
                                       "b",
                                       EqLookupNode::LookupStrategy::kNestedLoopJoin,
                                       boost::none /* idxEntry */,
                                       false /* shouldProduceBson */);

    QuerySolution qs;
    qs.setRoot(std::move(parentEqLookupNode));

    // Both nodes reference the same foreign collection. Therefore, our output vector should contain
    // a single copy of that namespace.
    std::vector<NamespaceStringOrUUID> expectedNssVector{foreignColl};
    assertNamespaceVectorsAreEqual(qs.getAllSecondaryNamespaces(mainNss), expectedNssVector);
}

TEST(QuerySolutionTest, GetFirstNodeByTypeFindsFirstNodeWhenNested) {
    auto collScanNode = std::make_unique<CollectionScanNode>();
    auto limitNode = std::make_unique<LimitNode>(
        std::move(collScanNode), 10ll, LimitSkipParameterization::Disabled);

    QuerySolution qs;
    qs.setRoot(std::move(limitNode));

    auto [foundNode, foundCount] = qs.getFirstNodeByType(StageType::STAGE_COLLSCAN);
    ASSERT_EQ(qs.root()->children.at(0).get(), foundNode);
    ASSERT_EQ(1, foundCount);
}

TEST(QuerySolutionTest, GetFirstNodeByTypeFindsFirstNodeWhenInRoot) {
    auto collScanNode = std::make_unique<CollectionScanNode>();

    QuerySolution qs;
    qs.setRoot(std::move(collScanNode));

    auto [foundNode, foundCount] = qs.getFirstNodeByType(StageType::STAGE_COLLSCAN);
    ASSERT_EQ(qs.root(), foundNode);
    ASSERT_EQ(1, foundCount);
}

TEST(QuerySolutionTest, GetFirstNodeByTypeReturnsNullIfNotFound) {
    auto collScanNode = std::make_unique<CollectionScanNode>();

    QuerySolution qs;
    qs.setRoot(std::move(collScanNode));

    auto [foundNode, foundCount] = qs.getFirstNodeByType(StageType::STAGE_IXSCAN);
    ASSERT_EQ(nullptr, foundNode);
    ASSERT_EQ(0, foundCount);
}

TEST(QuerySolutionTest, GetFirstNodeByTypeFindsFirstAndCountsWhenSeveral) {
    auto collScanNode = std::make_unique<CollectionScanNode>();
    auto firstLimitNode = std::make_unique<LimitNode>(
        std::move(collScanNode), 10ll, LimitSkipParameterization::Disabled);
    auto firstLimitNodeLimitValue = firstLimitNode->limit;
    auto secondLimitNode = std::make_unique<LimitNode>(
        std::move(firstLimitNode), 8ll, LimitSkipParameterization::Disabled);
    // We use its 'limit' value to assert the first one was retrieved below hence cannot be equals
    ASSERT(firstLimitNodeLimitValue != secondLimitNode->limit);
    auto skipNode = std::make_unique<SkipNode>(
        std::move(secondLimitNode), 9ll, LimitSkipParameterization::Disabled);

    QuerySolution qs;
    qs.setRoot(std::move(skipNode));

    auto [foundNode, foundCount] = qs.getFirstNodeByType(StageType::STAGE_LIMIT);
    const LimitNode* foundLimitNode = dynamic_cast<const LimitNode*>(foundNode);
    ASSERT(foundLimitNode);
    ASSERT_EQ(foundLimitNode->limit, 8ll);  // 8 is the value we assign to the first limit node
    ASSERT_EQ(2, foundCount);
}

TEST(QuerySolutionTest, ShouldCacheEofPlanTree) {
    // QuerySolutions with EOF nodes are eligible for the plan cache.

    // QuerySolution with root EOF node is eligible for the plan cache.
    auto solution1 = std::make_unique<QuerySolution>();
    solution1->setRoot(std::make_unique<EofNode>(eof_node::EOFType::PredicateEvalsToFalse));
    ASSERT_TRUE(solution1->isEligibleForPlanCache());

    // QuerySolution with child EOF node is eligible for the plan cache.
    std::vector<std::unique_ptr<QuerySolutionNode>> indexScanList;
    indexScanList.push_back(std::make_unique<EofNode>(eof_node::EOFType::NonExistentNamespace));
    auto orNode = std::make_unique<OrNode>();
    orNode->addChildren(std::move(indexScanList));

    auto solution2 = std::make_unique<QuerySolution>();
    solution2->setRoot(std::move(orNode));

    ASSERT_TRUE(solution2->isEligibleForPlanCache());
}
}  // namespace

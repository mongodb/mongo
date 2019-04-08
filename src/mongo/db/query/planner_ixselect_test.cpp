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

/**
 * This file contains tests for mongo/db/query/planner_ixselect.cpp
 */

#include "mongo/db/query/planner_ixselect.h"

#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/text.h"
#include <memory>

using namespace mongo;

namespace {

constexpr CollatorInterface* kSimpleCollator = nullptr;

using std::unique_ptr;
using std::string;
using std::vector;

/**
 * Utility function to create MatchExpression
 */
unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status = MatchExpressionParser::parse(obj, std::move(expCtx));
    ASSERT_TRUE(status.isOK());
    return std::move(status.getValue());
}

/**
 * Utility function to join elements in iterator range with comma
 */
template <typename Iter>
string toString(Iter begin, Iter end) {
    str::stream ss;
    ss << "[";
    for (Iter i = begin; i != end; i++) {
        if (i != begin) {
            ss << " ";
        }
        ss << *i;
    }
    ss << "]";
    return ss;
}

/**
 * Test function for getFields()
 * Parses query string to obtain MatchExpression which is passed together with prefix
 * to QueryPlannerIXSelect::getFields()
 * Results are compared with expected fields (parsed from expectedFieldsStr)
 */
void testGetFields(const char* query, const char* prefix, const char* expectedFieldsStr) {
    BSONObj obj = fromjson(query);
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    stdx::unordered_set<string> fields;
    QueryPlannerIXSelect::getFields(expr.get(), prefix, &fields);

    // Verify results
    // First, check that results contain a superset of expected fields.
    vector<string> expectedFields = StringSplitter::split(expectedFieldsStr, ",");
    for (vector<string>::const_iterator i = expectedFields.begin(); i != expectedFields.end();
         i++) {
        if (fields.find(*i) == fields.end()) {
            str::stream ss;
            ss << "getFields(query=" << query << ", prefix=" << prefix << "): unable to find " << *i
               << " in result: " << toString(fields.begin(), fields.end());
            FAIL(ss);
        }
    }

    // Next, confirm that results do not contain any unexpected fields.
    if (fields.size() != expectedFields.size()) {
        str::stream ss;
        ss << "getFields(query=" << query << ", prefix=" << prefix
           << "): unexpected fields in result. expected: "
           << toString(expectedFields.begin(), expectedFields.end())
           << ". actual: " << toString(fields.begin(), fields.end());
        FAIL(ss);
    }
}

/**
 * Basic test cases for getFields()
 * Includes logical operators
 */
TEST(QueryPlannerIXSelectTest, GetFieldsBasic) {
    // Arguments to test function: query, prefix, comma-delimited list of expected fields
    testGetFields("{}", "", "");
    testGetFields("{a: 1}", "", "a");
    testGetFields("{a: 1}", "c.", "c.a");
    testGetFields("{a: 1, b: 1}", "", "a,b");
    testGetFields("{a: {$in: [1]}}", "", "a");
    testGetFields("{$or: [{a: 1}, {b: 1}]}", "", "a,b");
}

/**
 * Array test cases for getFields
 */
TEST(QueryPlannerIXSelectTest, GetFieldsArray) {
    testGetFields("{a: {$elemMatch: {b: 1}}}", "", "a.b");
    testGetFields("{a: {$all: [{$elemMatch: {b: 1}}]}}", "", "a.b");
}

/**
 * Negation test cases for getFields()
 * $ne, $nin, $nor
 */
TEST(QueryPlannerIXSelectTest, GetFieldsNegation) {
    testGetFields("{a: {$ne: 1}}", "", "a");
    testGetFields("{a: {$nin: [1]}}", "", "a");
    testGetFields("{$nor: [{a: 1}, {b: 1}]}", "", "");
    testGetFields("{$and: [{a: 1}, {a: {$ne: 2}}]}", "", "a");
}

/**
 * Array negation test cases for getFields
 */
TEST(QueryPlannerIXSelectTest, GetFieldsArrayNegation) {
    testGetFields("{a: {$elemMatch: {b: {$ne: 1}}}}", "", "a.b");
    testGetFields("{a: {$all: [{$elemMatch: {b: {$ne: 1}}}]}}", "", "a.b");
}

/**
 * Performs a pre-order traversal of expression tree. Validates
 * that all tagged nodes contain an instance of RelevantTag.
 * Finds all indices included in RelevantTags, and returns them in the 'indices' out-parameter.
 */
void findRelevantTaggedNodePathsAndIndices(MatchExpression* root,
                                           vector<string>* paths,
                                           std::set<size_t>* indices) {
    MatchExpression::TagData* tag = root->getTag();
    if (tag) {
        StringBuilder buf;
        tag->debugString(&buf);
        RelevantTag* r = dynamic_cast<RelevantTag*>(tag);
        if (!r) {
            str::stream ss;
            ss << "tag is not instance of RelevantTag. tree: " << root->debugString()
               << "; tag: " << buf.str();
            FAIL(ss);
        }
        paths->push_back(r->path);
        for (auto const& index : r->first) {
            indices->insert(index);
        }
        for (auto const& index : r->notFirst) {
            indices->insert(index);
        }
    }
    for (size_t i = 0; i < root->numChildren(); ++i) {
        findRelevantTaggedNodePathsAndIndices(root->getChild(i), paths, indices);
    }
}

/**
 * Make a minimal IndexEntry from just a key pattern. A dummy name will be added.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
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

/**
 * Parses a MatchExpression from query string and passes that along with prefix, collator, and
 * indices to rateIndices. Verifies results against list of expected paths and expected indices. In
 * future, we may expand this test function to validate which indices are assigned to which node.
 */
void testRateIndices(const char* query,
                     const char* prefix,
                     const CollatorInterface* collator,
                     const vector<IndexEntry>& indices,
                     const char* expectedPathsStr,
                     const std::set<size_t>& expectedIndices) {
    // Parse and rate query. Some of the nodes in the rated tree
    // will be tagged after the rating process.
    BSONObj obj = fromjson(query);
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));

    QueryPlannerIXSelect::rateIndices(expr.get(), prefix, indices, collator);

    // Retrieve a list of paths and a set of indices embedded in
    // tagged nodes.
    vector<string> paths;
    std::set<size_t> actualIndices;
    findRelevantTaggedNodePathsAndIndices(expr.get(), &paths, &actualIndices);

    // Compare the expected indices with the actual indices.
    if (actualIndices != expectedIndices) {
        str::stream ss;
        ss << "rateIndices(query=" << query << ", prefix=" << prefix
           << "): expected indices did not match actual indices. expected: "
           << toString(expectedIndices.begin(), expectedIndices.end())
           << ". actual: " << toString(actualIndices.begin(), actualIndices.end());
        FAIL(ss);
    }

    // Compare with expected list of paths.
    // First verify number of paths retrieved.
    vector<string> expectedPaths = StringSplitter::split(expectedPathsStr, ",");
    if (paths.size() != expectedPaths.size()) {
        str::stream ss;
        ss << "rateIndices(query=" << query << ", prefix=" << prefix
           << "): unexpected number of tagged nodes found. expected: "
           << toString(expectedPaths.begin(), expectedPaths.end())
           << ". actual: " << toString(paths.begin(), paths.end());
        FAIL(ss);
    }

    // Next, check that value and order of each element match between the two lists.
    for (vector<string>::const_iterator i = paths.begin(), j = expectedPaths.begin();
         i != paths.end();
         i++, j++) {
        if (*i == *j) {
            continue;
        }
        str::stream ss;
        ss << "rateIndices(query=" << query << ", prefix=" << prefix
           << "): unexpected path found. expected: " << *j << " "
           << toString(expectedPaths.begin(), expectedPaths.end()) << ". actual: " << *i << " "
           << toString(paths.begin(), paths.end());
        FAIL(ss);
    }
}

/**
 * Calls testRateIndices with an empty set of indices and a null collation, so we only test which
 * nodes are tagged.
 */
void testRateIndicesTaggedNodePaths(const char* query,
                                    const char* prefix,
                                    const char* expectedPathsStr) {
    // Currently, we tag every indexable node even when no compatible
    // index is available. Hence, it is fine to pass an empty vector of
    // indices to rateIndices().
    vector<IndexEntry> indices;
    std::set<size_t> expectedIndices;
    testRateIndices(query, prefix, nullptr, indices, expectedPathsStr, expectedIndices);
}

/**
 * Basic test cases for rateIndices().
 * Includes logical operators.
 */
TEST(QueryPlannerIXSelectTest, RateIndicesTaggedNodePathsBasic) {
    // Test arguments: query, prefix, comma-delimited list of expected paths
    testRateIndicesTaggedNodePaths("{}", "", "");
    testRateIndicesTaggedNodePaths("{a: 1}", "", "a");
    testRateIndicesTaggedNodePaths("{a: 1}", "c.", "c.a");
    testRateIndicesTaggedNodePaths("{a: 1, b: 1}", "", "a,b");
    testRateIndicesTaggedNodePaths("{a: {$in: [1]}}", "", "a");
    testRateIndicesTaggedNodePaths("{$or: [{a: 1}, {b: 1}]}", "", "a,b");
}

/**
 * Array test cases for rateIndices().
 */
TEST(QueryPlannerIXSelectTest, RateIndicesTaggedNodePathArray) {
    testRateIndicesTaggedNodePaths("{a: {$elemMatch: {b: 1}}}", "", "a.b");
    testRateIndicesTaggedNodePaths("{a: {$all: [{$elemMatch: {b: 1}}]}}", "", "a.b");
}

/**
 * Negation test cases for rateIndices().
 */
TEST(QueryPlannerIXSelectTest, RateIndicesTaggedNodePathsNegation) {
    testRateIndicesTaggedNodePaths("{a: {$ne: 1}}", "", "a,a");
    testRateIndicesTaggedNodePaths("{a: {$nin: [1]}}", "", "a,a");
    testRateIndicesTaggedNodePaths("{$nor: [{a: 1}, {b: 1}]}", "", "");
    testRateIndicesTaggedNodePaths("{$and: [{a: 1}, {a: {$ne: 2}}]}", "", "a,a,a");
}

/**
 * Array negation test cases for rateIndices().
 */
TEST(QueryPlannerIXSelectTest, RateIndicesTaggedNodePathArrayNegation) {
    testRateIndicesTaggedNodePaths("{a: {$elemMatch: {b: {$ne: 1}}}}", "", "a.b,a.b");
    testRateIndicesTaggedNodePaths("{a: {$all: [{$elemMatch: {b: {$ne: 1}}}]}}", "", "a.b,a.b");
}

/**
 * $not within $elemMatch should not attempt to use a sparse index for $exists:false.
 */
TEST(QueryPlannerIXSelectTest, ElemMatchNotExistsShouldNotUseSparseIndex) {
    std::vector<IndexEntry> indices;
    auto idxEntry = buildSimpleIndexEntry(BSON("a" << 1));
    idxEntry.sparse = true;
    indices.push_back(idxEntry);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$elemMatch: {$not: {$exists: true}}}}",
                    "",
                    kSimpleCollator,
                    indices,
                    "",
                    expectedIndices);
}

/**
 * $in with a null value within $elemMatch can use a sparse index.
 */
TEST(QueryPlannerIXSelectTest, ElemMatchInNullValueShouldUseSparseIndex) {
    std::vector<IndexEntry> indices;
    auto idxEntry = buildSimpleIndexEntry(BSON("a" << 1));
    idxEntry.sparse = true;
    indices.push_back(idxEntry);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$elemMatch: {$in: [null]}}}", "", kSimpleCollator, indices, "a", expectedIndices);
}

/**
 * $geo queries within $elemMatch should not use a normal B-tree index.
 */
TEST(QueryPlannerIXSelectTest, ElemMatchGeoShouldNotUseBtreeIndex) {
    std::vector<IndexEntry> indices;
    auto idxEntry = buildSimpleIndexEntry(BSON("a" << 1));
    indices.push_back(idxEntry);
    std::set<size_t> expectedIndices;
    testRateIndices(R"({a: {$elemMatch: {$geoWithin: {$geometry: {type: 'Polygon',
                      coordinates: [[[0,0],[0,1],[1,0],[0,0]]]}}}}})",
                    "",
                    kSimpleCollator,
                    indices,
                    "a",
                    expectedIndices);
}

/**
 * $eq with a null value within $elemMatch can use a sparse index.
 */
TEST(QueryPlannerIXSelectTest, ElemMatchEqNullValueShouldUseSparseIndex) {
    std::vector<IndexEntry> indices;
    auto idxEntry = buildSimpleIndexEntry(BSON("a" << 1));
    idxEntry.sparse = true;
    indices.push_back(idxEntry);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$elemMatch: {$eq: null}}}", "", kSimpleCollator, indices, "a", expectedIndices);
}

/**
 * $elemMatch with multiple children will not use an index if any child is incompatible.
 */
TEST(QueryPlannerIXSelectTest, ElemMatchMultipleChildrenShouldRequireAllToBeCompatible) {
    std::vector<IndexEntry> indices;
    auto idxEntry = buildSimpleIndexEntry(BSON("a" << 1));
    idxEntry.sparse = true;
    indices.push_back(idxEntry);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$elemMatch: {$eq: null, $not: {$exists: true}}}}",
                    "",
                    kSimpleCollator,
                    indices,
                    "",
                    expectedIndices);
}

/**
 * If the collator is null, we select the relevant index with a null collator.
 */
TEST(QueryPlannerIXSelectTest, NullCollatorsMatch) {
    std::vector<IndexEntry> indices;
    indices.push_back(buildSimpleIndexEntry(BSON("a" << 1)));
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: 'string'}", "", nullptr, indices, "a", expectedIndices);
}

/**
 * If the collator is not null, we do not select the relevant index with a null collator.
 */
TEST(QueryPlannerIXSelectTest, NonNullCollatorDoesNotMatchIndexWithNullCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    std::vector<IndexEntry> indices;
    indices.push_back(buildSimpleIndexEntry(BSON("a" << 1)));
    std::set<size_t> expectedIndices;
    testRateIndices("{a: 'string'}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If the collator is null, we do not select the relevant index with a non-null collator.
 */
TEST(QueryPlannerIXSelectTest, NullCollatorDoesNotMatchIndexWithNonNullCollator) {
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: 'string'}", "", nullptr, indices, "a", expectedIndices);
}

/**
 * If the collator is non-null, we select the relevant index with an equal collator.
 */
TEST(QueryPlannerIXSelectTest, EqualCollatorsMatch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: 'string'}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If the collator is non-null, we do not select the relevant index with an unequal collator.
 */
TEST(QueryPlannerIXSelectTest, UnequalCollatorsDoNotMatch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: 'string'}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If no string comparison is done, unequal collators are allowed.
 */
TEST(QueryPlannerIXSelectTest, NoStringComparison) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: 1}", "", &collator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, StringInternalExprEqUnequalCollatorsCannotUseIndex) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices(
        "{a: {$_internalExprEq: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, StringInternalExprEqEqualCollatorsCanUseIndex) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$_internalExprEq: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, NestedObjectInternalExprEqUnequalCollatorsCannotUseIndex) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices(
        "{a: {$_internalExprEq: {b: 'string'}}}", "", &collator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, NestedObjectInternalExprEqEqualCollatorsCanUseIndex) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$_internalExprEq: {b: 'string'}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $gt string comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, StringGTUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$gt: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $gt string comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, StringGTEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$gt: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $gt array comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, ArrayGTUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$gt: ['string']}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $gt array comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, ArrayGTEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$gt: ['string']}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $gt object comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, NestedObjectGTUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$gt: {b: 'string'}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $gt object comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, NestedObjectGTEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$gt: {b : 'string'}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $gte string comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, StringGTEUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$gte: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $gte string comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, StringGTEEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$gte: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $lt string comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, StringLTUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$lt: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $lt string comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, StringLTEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$lt: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $lte string comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, StringLTEUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$lte: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $lte string comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, StringLTEEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$lte: 'string'}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If no string comparison is done in an 'in' expression, unequal collators are allowed.
 */
TEST(QueryPlannerIXSelectTest, NoStringComparisonInExpression) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$in: [1, 2]}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an 'in' expression, matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonInExpressionUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$in: [1, 2, 'b', 3]}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an 'in' expression, matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonInExpressionEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$in: [1, 2, 'b', 3]}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If no string comparison is done in a 'not' expression, unequal collators are allowed.
 */
TEST(QueryPlannerIXSelectTest, NoStringComparisonNotExpression) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$not: {$gt: 1}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in a 'not' expression, matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonNotExpressionUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$not: {$gt: 'a'}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in a 'not' expression, matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonNotExpressionEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$not: {$gt: 'a'}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If no string comparison is done in an elemMatch value, unequal collators are allowed.
 */
TEST(QueryPlannerIXSelectTest, NoStringComparisonElemMatchValueExpression) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$elemMatch: {$gt: 1}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an elemMatch value, matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonElemMatchValueExpressionUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices(
        "{a: {$elemMatch: {$gt: 'string'}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an elemMatch value, matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonElemMatchValueExpressionEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$elemMatch: {$gt: 'string'}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If no string comparison is done in an 'in' in a 'not', unequal collators are allowed.
 */
TEST(QueryPlannerIXSelectTest, NoStringComparisonNotInExpression) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$not: {$in: [1]}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an 'in' in a 'not', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonNotInExpressionUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$not: {$in: ['a']}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an 'in' in a 'not', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonNotInExpressionEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$not: {$in: ['a']}}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If no string comparison is done in a 'nin', unequal collators are allowed.
 */
TEST(QueryPlannerIXSelectTest, NoStringComparisonNinExpression) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$nin: [1]}}", "", &collator, indices, "a,a", expectedIndices);
}

/**
 * If string comparison is done in a 'nin', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonNinExpressionUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$nin: ['a']}}", "", &collator, indices, "a,a", expectedIndices);
}

/**
 * If string comparison is done in a 'nin', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonNinExpressionEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$nin: ['a']}}", "", &collator, indices, "a,a", expectedIndices);
}

/**
 * If string comparison is done in an 'or', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonOrExpressionUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{$or: [{a: 'string'}]}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an 'or', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonOrExpressionEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{$or: [{a: 'string'}]}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an 'and', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonAndExpressionUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{$and: [{a: 'string'}]}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an 'and', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonAndExpressionEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{$and: [{a: 'string'}]}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an 'all', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonAllExpressionUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices("{a: {$all: ['string']}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an 'all', matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonAllExpressionEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$all: ['string']}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If string comparison is done in an elemMatch object, matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonElemMatchObjectExpressionUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a.b" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices;
    testRateIndices(
        "{a: {$elemMatch: {b: 'string'}}}", "", &collator, indices, "a.b", expectedIndices);
}

/**
 * If string comparison is done in an elemMatch object, matching collators are required.
 */
TEST(QueryPlannerIXSelectTest, StringComparisonElemMatchObjectExpressionEqualCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a.b" << 1));
    index.collator = &collator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$elemMatch: {b: 'string'}}}", "", &collator, indices, "a.b", expectedIndices);
}

/**
 * If no string comparison is done in a query containing $mod, unequal collators are allowed.
 */
TEST(QueryPlannerIXSelectTest, NoStringComparisonMod) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$mod: [2, 0]}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If no string comparison is done in a query containing $exists, unequal collators are allowed.
 */
TEST(QueryPlannerIXSelectTest, NoStringComparisonExists) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$exists: true}}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If no string comparison is done in a query containing $type, unequal collators are allowed.
 */
TEST(QueryPlannerIXSelectTest, NoStringComparisonType) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto index = buildSimpleIndexEntry(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    std::set<std::string> testPatterns = {
        "{a: {$type: 'string'}}", "{a: {$type: 'object'}}", "{a: {$type: 'array'}}"};
    for (const auto& pattern : testPatterns) {
        testRateIndices(pattern.c_str(), "", &collator, indices, "a", expectedIndices);
    }
}

// Helper which constructs an IndexEntry and returns it along with an owned ProjectionExecAgg, which
// is non-null if the requested entry represents a wildcard index and null otherwise. When non-null,
// it simulates the ProjectionExecAgg that is owned by the $** IndexAccessMethod.
auto makeIndexEntry(BSONObj keyPattern,
                    MultikeyPaths multiKeyPaths,
                    std::set<FieldRef> multiKeyPathSet = {},
                    BSONObj infoObj = BSONObj()) {
    auto projExec = (keyPattern.firstElement().fieldNameStringData().endsWith("$**"_sd)
                         ? WildcardKeyGenerator::createProjectionExec(
                               keyPattern, infoObj.getObjectField("wildcardProjection"))
                         : nullptr);

    auto multiKey = !multiKeyPathSet.empty() ||
        std::any_of(multiKeyPaths.cbegin(), multiKeyPaths.cend(), [](const auto& entry) {
            return !entry.empty();
        });
    return std::make_pair(IndexEntry(keyPattern,
                                     IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                                     multiKey,
                                     multiKeyPaths,
                                     multiKeyPathSet,
                                     false,
                                     false,
                                     CoreIndexInfo::Identifier("test_foo"),
                                     nullptr,
                                     {},
                                     nullptr,
                                     projExec.get()),
                          std::move(projExec));
}

TEST(QueryPlannerIXSelectTest, InternalExprEqCannotUseMultiKeyIndex) {
    auto entry = makeIndexEntry(BSON("a" << 1), {{0U}});
    std::vector<IndexEntry> indices;
    indices.push_back(entry.first);
    std::set<size_t> expectedIndices;
    testRateIndices(
        "{a: {$_internalExprEq: 1}}", "", kSimpleCollator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, InternalExprEqCanUseNonMultikeyFieldOfMultikeyIndex) {
    auto entry = makeIndexEntry(BSON("a" << 1 << "b" << 1), {{0U}, {}});
    std::vector<IndexEntry> indices;
    indices.push_back(entry.first);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{b: {$_internalExprEq: 1}}", "", kSimpleCollator, indices, "b", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, InternalExprEqCannotUseMultikeyIndexWithoutPathLevelMultikeyData) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1));
    entry.multikey = true;
    std::vector<IndexEntry> indices;
    indices.push_back(entry);
    std::set<size_t> expectedIndices;
    testRateIndices(
        "{a: {$_internalExprEq: 1}}", "", kSimpleCollator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, InternalExprEqCanUseNonMultikeyIndexWithNoPathLevelMultikeyData) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1));
    std::vector<IndexEntry> indices;
    indices.push_back(entry);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$_internalExprEq: 1}}", "", kSimpleCollator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, InternalExprEqCanUseHashedIndex) {
    auto entry = buildSimpleIndexEntry(BSON("a"
                                            << "hashed"));
    std::vector<IndexEntry> indices;
    indices.push_back(entry);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$_internalExprEq: 1}}", "", kSimpleCollator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, InternalExprEqCannotUseTextIndexPrefix) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1 << "_fts"
                                                << "text"
                                                << "_ftsx"
                                                << 1));
    std::vector<IndexEntry> indices;
    indices.push_back(entry);
    std::set<size_t> expectedIndices;
    testRateIndices(
        "{a: {$_internalExprEq: 1}}", "", kSimpleCollator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, InternalExprEqCanUseTextIndexSuffix) {
    auto entry = buildSimpleIndexEntry(BSON("_fts"
                                            << "text"
                                            << "_ftsx"
                                            << 1
                                            << "a"
                                            << 1));
    std::vector<IndexEntry> indices;
    indices.push_back(entry);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$_internalExprEq: 1}}", "", kSimpleCollator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, InternalExprEqCanUseSparseIndexWithComparisonToNull) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1));
    entry.sparse = true;
    std::vector<IndexEntry> indices;
    indices.push_back(entry);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$_internalExprEq: null}}", "", kSimpleCollator, indices, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, InternalExprEqCanUseSparseIndexWithComparisonToNonNull) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1));
    entry.sparse = true;
    std::vector<IndexEntry> indices;
    indices.push_back(entry);
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$_internalExprEq: 1}}", "", kSimpleCollator, indices, "a", expectedIndices);
}
TEST(QueryPlannerIXSelectTest, NotEqualsNullCanUseIndex) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1));
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$ne: null}}", "", kSimpleCollator, {entry}, "a,a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, NotEqualsNullCannotUseMultiKeyIndex) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1));
    entry.multikey = true;
    std::set<size_t> expectedIndices = {};
    testRateIndices("{a: {$ne: null}}", "", kSimpleCollator, {entry}, "a,a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, NotEqualsNullCannotUseDottedMultiKeyIndex) {
    auto entry = buildSimpleIndexEntry(BSON("a.b" << 1));
    entry.multikeyPaths = {{0}};
    std::set<size_t> expectedIndices = {};
    testRateIndices(
        "{'a.b': {$ne: null}}", "", kSimpleCollator, {entry}, "a.b,a.b", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, NotEqualsNullCanUseIndexWhichIsMultiKeyOnAnotherPath) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1 << "mk" << 1));
    entry.multikeyPaths = {{}, {0}};
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$ne: null}}", "", kSimpleCollator, {entry}, "a,a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, ElemMatchValueWithNotEqualsNullCanUseIndex) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1));
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$elemMatch: {$ne: null}}}", "", kSimpleCollator, {entry}, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, ElemMatchValueWithNotEqualsNullCanUseMultiKeyIndex) {
    auto entry = buildSimpleIndexEntry(BSON("a" << 1));
    entry.multikey = true;
    std::set<size_t> expectedIndices = {0};
    testRateIndices(
        "{a: {$elemMatch: {$ne: null}}}", "", kSimpleCollator, {entry}, "a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, ElemMatchObjectWithNotEqualNullCanUseIndex) {
    auto entry = buildSimpleIndexEntry(BSON("a.b" << 1));
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: {$elemMatch: {b: {$ne: null}}}}",
                    "",
                    kSimpleCollator,
                    {entry},
                    "a.b,a.b",
                    expectedIndices);
}

TEST(QueryPlannerIXSelectTest, ElemMatchObjectWithNotEqualNullCannotUseOldMultiKeyIndex) {
    auto entry = buildSimpleIndexEntry(BSON("a.b" << 1));
    entry.multikey = true;
    std::set<size_t> expectedIndices = {};
    testRateIndices("{a: {$elemMatch: {b: {$ne: null}}}}",
                    "",
                    kSimpleCollator,
                    {entry},
                    "a.b,a.b",
                    expectedIndices);
}

TEST(QueryPlannerIXSelectTest, ElemMatchObjectWithNotEqualNullCanUseIndexMultikeyOnPrefix) {
    auto entry = buildSimpleIndexEntry(BSON("a.b.c.d" << 1));
    entry.multikeyPaths = {{0U}};
    std::set<size_t> expectedIndices = {0U};
    const auto query = "{'a.b': {$elemMatch: {'c.d': {$ne: null}}}}";
    testRateIndices(query, "", kSimpleCollator, {entry}, "a.b.c.d,a.b.c.d", expectedIndices);

    entry.multikeyPaths = {{1U}};
    testRateIndices(query, "", kSimpleCollator, {entry}, "a.b.c.d,a.b.c.d", expectedIndices);

    entry.multikeyPaths = {{2U}};
    expectedIndices = {};
    testRateIndices(query, "", kSimpleCollator, {entry}, "a.b.c.d,a.b.c.d", expectedIndices);

    entry.multikeyPaths = {{3U}};
    expectedIndices = {};
    testRateIndices(query, "", kSimpleCollator, {entry}, "a.b.c.d,a.b.c.d", expectedIndices);
}

TEST(QueryPlannerIXSelectTest,
     NestedElemMatchObjectWithNotEqualNullCanUseIndexMultikeyOnAnyPrefix) {
    auto entry = buildSimpleIndexEntry(BSON("a.b.c.d" << 1));
    entry.multikeyPaths = {{0U}};
    std::set<size_t> expectedIndices = {0U};
    const auto query = "{a: {$elemMatch: {b: {$elemMatch: {'c.d': {$ne: null}}}}}}";
    testRateIndices(query, "", kSimpleCollator, {entry}, "a.b.c.d,a.b.c.d", expectedIndices);

    entry.multikeyPaths = {{1U}};
    testRateIndices(query, "", kSimpleCollator, {entry}, "a.b.c.d,a.b.c.d", expectedIndices);

    entry.multikeyPaths = {{2U}};
    expectedIndices = {};
    testRateIndices(query, "", kSimpleCollator, {entry}, "a.b.c.d,a.b.c.d", expectedIndices);

    entry.multikeyPaths = {{3U}};
    expectedIndices = {};
    testRateIndices(query, "", kSimpleCollator, {entry}, "a.b.c.d,a.b.c.d", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, HashedIndexShouldNotBeRelevantForNotPredicate) {
    auto entry = buildSimpleIndexEntry(BSON("a"
                                            << "hashed"));
    entry.type = IndexType::INDEX_HASHED;
    std::set<size_t> expectedIndices = {};
    testRateIndices("{a: {$ne: 4}}", "", kSimpleCollator, {entry}, "a,a", expectedIndices);
}

TEST(QueryPlannerIXSelectTest, HashedIndexShouldNotBeRelevantForNotEqualsNullPredicate) {
    auto entry = buildSimpleIndexEntry(BSON("a"
                                            << "hashed"));
    entry.type = IndexType::INDEX_HASHED;
    std::set<size_t> expectedIndices = {};
    testRateIndices("{a: {$ne: null}}", "", kSimpleCollator, {entry}, "a,a", expectedIndices);
}

/*
 * Will compare 'keyPatterns' with 'entries'. As part of comparing, it will sort both of them.
 */
bool indexEntryKeyPatternsMatch(vector<BSONObj>* keyPatterns, vector<IndexEntry>* entries) {
    ASSERT_EQ(entries->size(), keyPatterns->size());

    const auto cmpFn = [](const IndexEntry& a, const IndexEntry& b) {
        return SimpleBSONObjComparator::kInstance.evaluate(a.keyPattern < b.keyPattern);
    };

    std::sort(entries->begin(), entries->end(), cmpFn);
    std::sort(keyPatterns->begin(), keyPatterns->end(), [](const BSONObj& a, const BSONObj& b) {
        return SimpleBSONObjComparator::kInstance.evaluate(a < b);
    });

    return std::equal(keyPatterns->begin(),
                      keyPatterns->end(),
                      entries->begin(),
                      [](const BSONObj& keyPattern, const IndexEntry& ie) -> bool {
                          return SimpleBSONObjComparator::kInstance.evaluate(keyPattern ==
                                                                             ie.keyPattern);
                      });
}

TEST(QueryPlannerIXSelectTest, ExpandWildcardIndices) {
    const auto indexEntry = makeIndexEntry(BSON("$**" << 1), {});

    // Case where no fields are specified.
    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(stdx::unordered_set<string>(), {indexEntry.first});
    ASSERT_TRUE(result.empty());

    stdx::unordered_set<string> fields = {"fieldA", "fieldB"};

    result = QueryPlannerIXSelect::expandIndexes(fields, {indexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {BSON("fieldA" << 1), BSON("fieldB" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));

    const auto wildcardIndexWithSubpath = makeIndexEntry(BSON("a.b.$**" << 1), {});
    fields = {"a.b", "a.b.c", "a.d"};
    result = QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexWithSubpath.first});
    expectedKeyPatterns = {BSON("a.b" << 1), BSON("a.b.c" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, ExpandWildcardIndicesInPresenceOfOtherIndices) {
    auto wildcardIndexEntry = makeIndexEntry(BSON("$**" << 1), {});
    auto aIndexEntry = makeIndexEntry(BSON("fieldA" << 1), {});
    auto bIndexEntry = makeIndexEntry(BSON("fieldB" << 1), {});
    auto abIndexEntry = makeIndexEntry(BSON("fieldA" << 1 << "fieldB" << 1), {});

    const stdx::unordered_set<string> fields = {"fieldA", "fieldB", "fieldC"};
    std::vector<BSONObj> expectedKeyPatterns = {
        BSON("fieldA" << 1), BSON("fieldA" << 1), BSON("fieldB" << 1), BSON("fieldC" << 1)};

    auto result =
        QueryPlannerIXSelect::expandIndexes(fields, {aIndexEntry.first, wildcardIndexEntry.first});
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
    result.clear();

    expectedKeyPatterns = {
        BSON("fieldB" << 1), BSON("fieldA" << 1), BSON("fieldB" << 1), BSON("fieldC" << 1)};
    result =
        QueryPlannerIXSelect::expandIndexes(fields, {bIndexEntry.first, wildcardIndexEntry.first});
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
    result.clear();

    result = QueryPlannerIXSelect::expandIndexes(
        fields, {aIndexEntry.first, wildcardIndexEntry.first, bIndexEntry.first});
    expectedKeyPatterns = {BSON("fieldA" << 1),
                           BSON("fieldA" << 1),
                           BSON("fieldB" << 1),
                           BSON("fieldC" << 1),
                           BSON("fieldB" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
    result.clear();

    result =
        QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first, abIndexEntry.first});
    expectedKeyPatterns = {BSON("fieldA" << 1),
                           BSON("fieldB" << 1),
                           BSON("fieldC" << 1),
                           BSON("fieldA" << 1 << "fieldB" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, ExpandedIndexEntriesAreCorrectlyMarkedAsMultikeyOrNonMultikey) {
    auto wildcardIndexEntry = makeIndexEntry(BSON("$**" << 1), {}, {FieldRef{"a"}});
    const stdx::unordered_set<string> fields = {"a.b", "c.d"};
    std::vector<BSONObj> expectedKeyPatterns = {BSON("a.b" << 1), BSON("c.d" << 1)};

    auto result = QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first});
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));

    for (auto&& entry : result) {
        ASSERT_TRUE(entry.multikeyPathSet.empty());
        ASSERT_EQ(entry.multikeyPaths.size(), 1u);

        if (SimpleBSONObjComparator::kInstance.evaluate(entry.keyPattern == BSON("a.b" << 1))) {
            ASSERT_TRUE(entry.multikey);
            ASSERT(entry.multikeyPaths[0] == std::set<std::size_t>{0u});
        } else {
            ASSERT_BSONOBJ_EQ(entry.keyPattern, BSON("c.d" << 1));
            ASSERT_FALSE(entry.multikey);
            ASSERT_TRUE(entry.multikeyPaths[0].empty());
        }
    }
}

TEST(QueryPlannerIXSelectTest, WildcardIndexExpansionExcludesIdField) {
    const auto indexEntry = makeIndexEntry(BSON("$**" << 1), {});

    stdx::unordered_set<string> fields = {"_id", "abc", "def"};

    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(fields, {indexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {BSON("abc" << 1), BSON("def" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, WildcardIndicesExpandedEntryHasCorrectProperties) {
    auto wildcardIndexEntry = makeIndexEntry(BSON("$**" << 1), {});
    wildcardIndexEntry.first.identifier = IndexEntry::Identifier("someIndex");

    stdx::unordered_set<string> fields = {"abc", "def"};

    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {BSON("abc" << 1), BSON("def" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));

    for (auto&& ie : result) {
        // A $** index entry is only multikey if it has a non-empty 'multikeyPathSet'. No such
        // multikey path set has been supplied by this test.
        ASSERT_FALSE(ie.multikey);
        ASSERT_TRUE(ie.multikeyPathSet.empty());
        ASSERT_EQ(ie.multikeyPaths.size(), 1u);
        ASSERT_TRUE(ie.multikeyPaths[0].empty());

        // Wildcard indices are always sparse.
        ASSERT_TRUE(ie.sparse);

        // Wildcard indexes are never unique.
        ASSERT_FALSE(ie.unique);

        ASSERT_EQ(ie.identifier,
                  IndexEntry::Identifier(wildcardIndexEntry.first.identifier.catalogName,
                                         ie.keyPattern.firstElementFieldName()));
    }
}

TEST(QueryPlannerIXSelectTest, WildcardIndicesExcludeNonMatchingKeySubpath) {
    auto wildcardIndexEntry = makeIndexEntry(BSON("subpath.$**" << 1), {});

    stdx::unordered_set<string> fields = {"abc", "def", "subpath.abc", "subpath.def", "subpath"};

    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {
        BSON("subpath.abc" << 1), BSON("subpath.def" << 1), BSON("subpath" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, WildcardIndicesExcludeNonMatchingPathsWithInclusionProjection) {
    auto wildcardIndexEntry =
        makeIndexEntry(BSON("$**" << 1),
                       {},
                       {},
                       BSON("wildcardProjection" << BSON("abc" << 1 << "subpath.abc" << 1)));

    stdx::unordered_set<string> fields = {"abc", "def", "subpath.abc", "subpath.def", "subpath"};

    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {BSON("abc" << 1), BSON("subpath.abc" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, WildcardIndicesExcludeNonMatchingPathsWithExclusionProjection) {
    auto wildcardIndexEntry =
        makeIndexEntry(BSON("$**" << 1),
                       {},
                       {},
                       BSON("wildcardProjection" << BSON("abc" << 0 << "subpath.abc" << 0)));

    stdx::unordered_set<string> fields = {"abc", "def", "subpath.abc", "subpath.def", "subpath"};

    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {
        BSON("def" << 1), BSON("subpath.def" << 1), BSON("subpath" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, WildcardIndicesWithInclusionProjectionAllowIdExclusion) {
    auto wildcardIndexEntry = makeIndexEntry(
        BSON("$**" << 1),
        {},
        {},
        BSON("wildcardProjection" << BSON("_id" << 0 << "abc" << 1 << "subpath.abc" << 1)));

    stdx::unordered_set<string> fields = {
        "_id", "abc", "def", "subpath.abc", "subpath.def", "subpath"};

    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {BSON("abc" << 1), BSON("subpath.abc" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, WildcardIndicesWithInclusionProjectionAllowIdInclusion) {
    auto wildcardIndexEntry = makeIndexEntry(
        BSON("$**" << 1),
        {},
        {},
        BSON("wildcardProjection" << BSON("_id" << 1 << "abc" << 1 << "subpath.abc" << 1)));

    stdx::unordered_set<string> fields = {
        "_id", "abc", "def", "subpath.abc", "subpath.def", "subpath"};

    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {
        BSON("_id" << 1), BSON("abc" << 1), BSON("subpath.abc" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, WildcardIndicesWithExclusionProjectionAllowIdInclusion) {
    auto wildcardIndexEntry = makeIndexEntry(
        BSON("$**" << 1),
        {},
        {},
        BSON("wildcardProjection" << BSON("_id" << 1 << "abc" << 0 << "subpath.abc" << 0)));

    stdx::unordered_set<string> fields = {
        "_id", "abc", "def", "subpath.abc", "subpath.def", "subpath"};

    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {
        BSON("_id" << 1), BSON("def" << 1), BSON("subpath.def" << 1), BSON("subpath" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, WildcardIndicesIncludeMatchingInternalNodes) {
    auto wildcardIndexEntry = makeIndexEntry(
        BSON("$**" << 1), {}, {}, BSON("wildcardProjection" << BSON("_id" << 1 << "subpath" << 1)));

    stdx::unordered_set<string> fields = {
        "_id", "abc", "def", "subpath.abc", "subpath.def", "subpath"};

    std::vector<IndexEntry> result =
        QueryPlannerIXSelect::expandIndexes(fields, {wildcardIndexEntry.first});
    std::vector<BSONObj> expectedKeyPatterns = {
        BSON("_id" << 1), BSON("subpath.abc" << 1), BSON("subpath.def" << 1), BSON("subpath" << 1)};
    ASSERT_TRUE(indexEntryKeyPatternsMatch(&expectedKeyPatterns, &result));
}

TEST(QueryPlannerIXSelectTest, WildcardIndexSupported) {
    ASSERT_FALSE(QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex(
        parseMatchExpression(fromjson("{x: {abc: 1}}")).get()));
    ASSERT_FALSE(QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex(
        parseMatchExpression(fromjson("{x: {$lt: {abc: 1}}}")).get()));
    ASSERT_FALSE(QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex(
        parseMatchExpression(fromjson("{x: {$in: [1, 2, 3, {abc: 1}]}}")).get()));
}

TEST(QueryPlannerIXSelectTest, WildcardIndexSupportedDoesNotTraverse) {
    // The function will not traverse a node's children.
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex(
        parseMatchExpression(fromjson("{$or: [{z: {abc: 1}}]}")).get()));
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex(
        parseMatchExpression(fromjson("{x: 5, y: {abc: 1}}")).get()));
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex(
        parseMatchExpression(fromjson("{x: {$ne: {abc: 1}}}")).get()));
}

TEST(QueryPlannerIXSelectTest, EqualityToEmptyObjectIsSupportedByWildcardIndex) {
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex(
        parseMatchExpression(fromjson("{x: {}}")).get()));
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex(
        parseMatchExpression(fromjson("{x: {$eq: {}}}")).get()));
}

TEST(QueryPlannerIXSelectTest, SparseIndexSupported) {
    auto filterAObj = fromjson("{x: null}");
    const auto queryA = parseMatchExpression(filterAObj);
    ASSERT_FALSE(QueryPlannerIXSelect::nodeIsSupportedBySparseIndex(queryA.get(), false));
    // When in an elemMatch, a comparison to null implies literal null, so it is always supported.
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedBySparseIndex(queryA.get(), true));

    auto filterBObj = fromjson("{x: {$in: [1, 2, 3, null]}}");
    const auto queryB = parseMatchExpression(filterBObj);
    ASSERT_FALSE(QueryPlannerIXSelect::nodeIsSupportedBySparseIndex(queryB.get(), false));
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedBySparseIndex(queryB.get(), true));
}

TEST(QueryPlannerIXSelectTest, SparseIndexSupportedDoesNotTraverse) {
    // The function will not traverse a node's children.
    const bool inElemMatch = false;
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedBySparseIndex(
        parseMatchExpression(fromjson("{$or: [{z: null}]}")).get(), inElemMatch));
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedBySparseIndex(
        parseMatchExpression(fromjson("{x: 5, y: null}")).get(), inElemMatch));
    ASSERT_TRUE(QueryPlannerIXSelect::nodeIsSupportedBySparseIndex(
        parseMatchExpression(fromjson("{x: {$ne: null}}")).get(), inElemMatch));
}

}  // namespace

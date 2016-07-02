/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/text.h"
#include <memory>

using namespace mongo;

namespace {

using std::unique_ptr;
using std::string;
using std::vector;

/**
 * Utility function to create MatchExpression
 */
unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(obj, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(status.isOK());
    return std::move(status.getValue());
}

/**
 * Utility function to join elements in iterator range with comma
 */
template <typename Iter>
string toString(Iter begin, Iter end) {
    mongoutils::str::stream ss;
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
    unordered_set<string> fields;
    QueryPlannerIXSelect::getFields(expr.get(), prefix, &fields);

    // Verify results
    // First, check that results contain a superset of expected fields.
    vector<string> expectedFields = StringSplitter::split(expectedFieldsStr, ",");
    for (vector<string>::const_iterator i = expectedFields.begin(); i != expectedFields.end();
         i++) {
        if (fields.find(*i) == fields.end()) {
            mongoutils::str::stream ss;
            ss << "getFields(query=" << query << ", prefix=" << prefix << "): unable to find " << *i
               << " in result: " << toString(fields.begin(), fields.end());
            FAIL(ss);
        }
    }

    // Next, confirm that results do not contain any unexpected fields.
    if (fields.size() != expectedFields.size()) {
        mongoutils::str::stream ss;
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
            mongoutils::str::stream ss;
            ss << "tag is not instance of RelevantTag. tree: " << root->toString()
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
        mongoutils::str::stream ss;
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
        mongoutils::str::stream ss;
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
        mongoutils::str::stream ss;
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
 * If the collator is null, we select the relevant index with a null collator.
 */
TEST(QueryPlannerIXSelectTest, NullCollatorsMatch) {
    std::vector<IndexEntry> indices;
    indices.push_back(IndexEntry(BSON("a" << 1)));
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: 'string'}", "", nullptr, indices, "a", expectedIndices);
}

/**
 * If the collator is not null, we do not select the relevant index with a null collator.
 */
TEST(QueryPlannerIXSelectTest, NonNullCollatorDoesNotMatchIndexWithNullCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    std::vector<IndexEntry> indices;
    indices.push_back(IndexEntry(BSON("a" << 1)));
    std::set<size_t> expectedIndices;
    testRateIndices("{a: 'string'}", "", &collator, indices, "a", expectedIndices);
}

/**
 * If the collator is null, we do not select the relevant index with a non-null collator.
 */
TEST(QueryPlannerIXSelectTest, NullCollatorDoesNotMatchIndexWithNonNullCollator) {
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    index.collator = &indexCollator;
    std::vector<IndexEntry> indices;
    indices.push_back(index);
    std::set<size_t> expectedIndices = {0};
    testRateIndices("{a: 1}", "", &collator, indices, "a", expectedIndices);
}

/**
 * $gt string comparison requires matching collator.
 */
TEST(QueryPlannerIXSelectTest, StringGTUnequalCollators) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a.b" << 1));
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
    IndexEntry index(BSON("a.b" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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
    IndexEntry index(BSON("a" << 1));
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

}  // namespace

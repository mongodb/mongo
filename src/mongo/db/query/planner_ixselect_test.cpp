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

#include <memory>
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/text.h"

using namespace mongo;

namespace {

    using std::unique_ptr;
    using std::string;
    using std::vector;

    /**
     * Utility function to create MatchExpression
     */
    MatchExpression* parseMatchExpression(const BSONObj& obj) {
        StatusWithMatchExpression status = MatchExpressionParser::parse(obj);
        ASSERT_TRUE(status.isOK());
        MatchExpression* expr(status.getValue());
        return expr;
    }

    /**
     * Utility function to join elements in iterator range with comma
     */
    template <typename Iter> string toString(Iter begin, Iter end) {
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
                ss << "getFields(query=" << query << ", prefix=" << prefix << "): unable to find "
                   << *i << " in result: " << toString(fields.begin(), fields.end());
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
     */
    void findRelevantTaggedNodePaths(MatchExpression* root, vector<string>* paths) {
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
        }
        for (size_t i = 0; i < root->numChildren(); ++i) {
            findRelevantTaggedNodePaths(root->getChild(i), paths);
        }
    }
 
    /**
     * Parses a MatchExpression from query string and passes that along with
     * prefix to rateIndices.
     * Verifies results against list of expected paths.
     * For now, we're only interested in which nodes are tagged.
     * In future, we may expand this test function to include
     * validate which indices are assigned to a node.
     */
    void testRateIndicesTaggedNodePaths(const char* query, const char* prefix,
                                        const char* expectedPathsStr) {
        // Parse and rate query. Some of the nodes in the rated tree
        // will be tagged after the rating process.
        BSONObj obj = fromjson(query);
        unique_ptr<MatchExpression> expr(parseMatchExpression(obj));

        // Currently, we tag every indexable node even when no compatible
        // index is available. Hence, it is fine to pass an empty vector of
        // indices to rateIndices().
        vector<IndexEntry> indices;
        QueryPlannerIXSelect::rateIndices(expr.get(), prefix, indices);

        // Retrieve a list of paths embedded in
        // tagged nodes.
        vector<string> paths;
        findRelevantTaggedNodePaths(expr.get(), &paths);

        // Compare with expected list of paths.
        // First verify number of paths retrieved.
        vector<string> expectedPaths = StringSplitter::split(expectedPathsStr, ",");
        if (paths.size() != expectedPaths.size()) {
            mongoutils::str::stream ss;
            ss << "rateIndices(query=" << query << ", prefix=" << prefix
               << "): unexpected number of tagged nodes found. expected: "
               << toString(expectedPaths.begin(), expectedPaths.end()) << ". actual: "
               << toString(paths.begin(), paths.end());
            FAIL(ss);
        }

        // Next, check that value and order of each element match between the two lists.
        for (vector<string>::const_iterator i = paths.begin(), j = expectedPaths.begin();
             i != paths.end(); i++, j++) {
            if (*i == *j) {
                continue;
            }
            mongoutils::str::stream ss;
            ss << "rateIndices(query=" << query << ", prefix=" << prefix
               << "): unexpected path found. expected: " << *j << " "
               << toString(expectedPaths.begin(), expectedPaths.end()) << ". actual: "
               << *i << " " << toString(paths.begin(), paths.end());
            FAIL(ss);
        }
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

}  // namespace

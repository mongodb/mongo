/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 * This file contains tests for mongo/db/query/query_planner.cpp that involve $text.
 */

#include "mongo/db/query/query_planner_test_lib.h"

#include <ostream>
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

    static const char* ns = "somebogusns";

    class QueryPlannerTest : public mongo::unittest::Test {
    protected:
        void setUp() {
            params.options = QueryPlannerParams::INCLUDE_COLLSCAN;
            addIndex(BSON("_id" << 1));
        }

        void tearDown() {
            delete cq;

            for (vector<QuerySolution*>::iterator it = solns.begin(); it != solns.end(); ++it) {
                delete *it;
            }
        }

        //
        // Build up test.
        //

        void addIndex(BSONObj keyPattern, bool multikey = false) {
            // The first false means not multikey.
            // The second false means not sparse.
            // The third arg is the index name and I am egotistical.
            params.indices.push_back(IndexEntry(keyPattern, multikey, false,
                                                "hari_king_of_the_stove"));
        }

        void addIndex(BSONObj keyPattern, bool multikey, bool sparse) {
            params.indices.push_back(IndexEntry(keyPattern, multikey, sparse,
                                                "note_to_self_dont_break_build"));
        }

        //
        // Execute planner.
        //

        void runQuery(BSONObj query) {
            runQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), 0, 0);
        }

        void runQuerySortProj(const BSONObj& query, const BSONObj& sort, const BSONObj& proj) {
            runQuerySortProjSkipLimit(query, sort, proj, 0, 0);
        }

        void runQuerySkipLimit(const BSONObj& query, long long skip, long long limit) {
            runQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), skip, limit);
        }

        void runQueryHint(const BSONObj& query, const BSONObj& hint) {
            runQuerySortProjSkipLimitHint(query, BSONObj(), BSONObj(), 0, 0, hint);
        }

        void runQuerySortProjSkipLimit(const BSONObj& query,
                                       const BSONObj& sort, const BSONObj& proj,
                                       long long skip, long long limit) {
            runQuerySortProjSkipLimitHint(query, sort, proj, skip, limit, BSONObj());
        }

        void runQuerySortHint(const BSONObj& query, const BSONObj& sort, const BSONObj& hint) {
            runQuerySortProjSkipLimitHint(query, sort, BSONObj(), 0, 0, hint);
        }

        void runQueryHintMinMax(const BSONObj& query, const BSONObj& hint,
                                const BSONObj& minObj, const BSONObj& maxObj) {

            runQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj, false);
        }

        void runQuerySortProjSkipLimitHint(const BSONObj& query,
                                           const BSONObj& sort, const BSONObj& proj,
                                           long long skip, long long limit,
                                           const BSONObj& hint) {
            runQueryFull(query, sort, proj, skip, limit, hint, BSONObj(), BSONObj(), false);
        }

        void runQuerySnapshot(const BSONObj& query) {
            runQueryFull(query, BSONObj(), BSONObj(), 0, 0, BSONObj(), BSONObj(),
                         BSONObj(), true);
        }

        void runQueryFull(const BSONObj& query,
                          const BSONObj& sort, const BSONObj& proj,
                          long long skip, long long limit,
                          const BSONObj& hint,
                          const BSONObj& minObj,
                          const BSONObj& maxObj,
                          bool snapshot) {
            solns.clear();
            Status s = CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, hint,
                                                    minObj, maxObj, snapshot, &cq);
            if (!s.isOK()) { cq = NULL; }
            ASSERT_OK(s);
            s = QueryPlanner::plan(*cq, params, &solns);
            ASSERT_OK(s);
        }

        /**
         * Same as runQuery* functions except we expect a failed status from the planning stage.
         */
        void runInvalidQuery(const BSONObj& query) {
            runInvalidQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), 0, 0);
        }

        void runInvalidQuerySortProj(const BSONObj& query, const BSONObj& sort,
                                     const BSONObj& proj) {
            runInvalidQuerySortProjSkipLimit(query, sort, proj, 0, 0);
        }

        void runInvalidQuerySortProjSkipLimit(const BSONObj& query,
                                              const BSONObj& sort, const BSONObj& proj,
                                              long long skip, long long limit) {
            runInvalidQuerySortProjSkipLimitHint(query, sort, proj, skip, limit, BSONObj());
        }

        void runInvalidQueryHint(const BSONObj& query, const BSONObj& hint) {
            runInvalidQuerySortProjSkipLimitHint(query, BSONObj(), BSONObj(), 0, 0, hint);
        }

        void runInvalidQueryHintMinMax(const BSONObj& query, const BSONObj& hint,
                                       const BSONObj& minObj, const BSONObj& maxObj) {
            runInvalidQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj, false);
        }

        void runInvalidQuerySortProjSkipLimitHint(const BSONObj& query,
                                                  const BSONObj& sort, const BSONObj& proj,
                                                  long long skip, long long limit,
                                                  const BSONObj& hint) {
            runInvalidQueryFull(query, sort, proj, skip, limit, hint, BSONObj(), BSONObj(), false);
        }

        void runInvalidQueryFull(const BSONObj& query,
                                 const BSONObj& sort, const BSONObj& proj,
                                 long long skip, long long limit,
                                 const BSONObj& hint,
                                 const BSONObj& minObj,
                                 const BSONObj& maxObj,
                                 bool snapshot) {
            solns.clear();
            Status s = CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, hint,
                                                    minObj, maxObj, snapshot, &cq);
            if (!s.isOK()) { cq = NULL; }
            ASSERT_OK(s);
            s = QueryPlanner::plan(*cq, params, &solns);
            ASSERT_NOT_OK(s);
        }

        //
        // Introspect solutions.
        //

        size_t getNumSolutions() const {
            return solns.size();
        }

        void dumpSolutions(mongoutils::str::stream& ost) const {
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                    it != solns.end();
                    ++it) {
                ost << (*it)->toString() << '\n';
            }
        }

        void dumpSolutions() const {
            mongoutils::str::stream ss;
            dumpSolutions(ss);
            cout << "Solutions: " << string(ss) << endl;
        }

        /**
         * Checks number solutions. Generates assertion message
         * containing solution dump if applicable.
         */
        void assertNumSolutions(size_t expectSolutions) const {
            if (getNumSolutions() == expectSolutions) {
                return;
            }
            mongoutils::str::stream ss;
            ss << "expected " << expectSolutions << " solutions but got " << getNumSolutions()
               << " instead. solutions generated: " << '\n';
            dumpSolutions(ss);
            FAIL(ss);
        }

        size_t numSolutionMatches(const string& solnJson) const {
            BSONObj testSoln = fromjson(solnJson);
            size_t matches = 0;
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                    it != solns.end();
                    ++it) {
                QuerySolutionNode* root = (*it)->root.get();
                if (QueryPlannerTestLib::solutionMatches(testSoln, root)) {
                    ++matches;
                }
            }
            return matches;
        }

        /**
         * Verifies that the solution tree represented in json by 'solnJson' is
         * one of the solutions generated by QueryPlanner.
         *
         * The number of expected matches, 'numMatches', could be greater than
         * 1 if solutions differ only by the pattern of index tags on a filter.
         */
        void assertSolutionExists(const string& solnJson, size_t numMatches = 1) const {
            size_t matches = numSolutionMatches(solnJson);
            if (numMatches == matches) {
                return;
            }
            mongoutils::str::stream ss;
            ss << "expected " << numMatches << " matches for solution " << solnJson
               << " but got " << matches
               << " instead. all solutions generated: " << '\n';
            dumpSolutions(ss);
            FAIL(ss);
        }

        /**
         * Given a vector of string-based solution tree representations 'solnStrs',
         * verifies that the query planner generated exactly one of these solutions.
         */
        void assertHasOneSolutionOf(const vector<string>& solnStrs) const {
            size_t matches = 0;
            for (vector<string>::const_iterator it = solnStrs.begin();
                    it != solnStrs.end();
                    ++it) {
                if (1U == numSolutionMatches(*it)) {
                    ++matches;
                }
            }
            if (1U == matches) {
                return;
            }
            mongoutils::str::stream ss;
            ss << "assertHasOneSolutionOf expected one matching solution"
               << " but got " << matches
               << " instead. all solutions generated: " << '\n';
            dumpSolutions(ss);
            FAIL(ss);
        }

        BSONObj queryObj;
        CanonicalQuery* cq;
        QueryPlannerParams params;
        vector<QuerySolution*> solns;
    };

    //
    // Text
    // Creating an FTS index {a:1, b:"text", c:1} actually
    // creates an index with spec {a:1, _fts: "text", _ftsx: 1, c:1}.
    // So, the latter is what we pass in to the planner.
    //
    // PS. You can also do {a:1, b:"text", d:"text", c:1} and it will create an index with the same
    // key pattern.
    //

    // Basic test that it works.
    TEST_F(QueryPlannerTest, SimpleText) {
        addIndex(BSON("_fts" << "text" << "_ftsx" << 1));
        runQuery(fromjson("{$text: {$search: 'blah'}}"));

        assertNumSolutions(1);
        assertSolutionExists("{text: {search: 'blah'}}");
    }

    // If you create an index {a:1, b: "text"} you can't use it for queries on just 'a'.
    TEST_F(QueryPlannerTest, CantUseTextUnlessHaveTextPred) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("a" << 1 << "_fts" << "text" << "_ftsx" << 1));
        runQuery(fromjson("{a:1}"));

        // No table scans allowed so there is no solution.
        assertNumSolutions(0);
    }

    // But if you create an index {a:1, b:"text"} you can use it if it has a pred on 'a'
    // and a text query.
    TEST_F(QueryPlannerTest, HaveOKPrefixOnTextIndex) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("a" << 1 << "_fts" << "text" << "_ftsx" << 1));

        runQuery(fromjson("{a:1, $text:{$search: 'blah'}}"));
        assertNumSolutions(1);
        assertSolutionExists("{text: {prefix: {a:1}, search: 'blah'}}}}");

        // TODO: Do we want to $or a collection scan with a text search?
        // runQuery(fromjson("{$or: [{b:1}, {a:1, $text: {$search: 'blah'}}]}"));
        // assertNumSolutions(1);

        runQuery(fromjson("{$or: [{_id:1}, {a:1, $text: {$search: 'blah'}}]}"));
        assertNumSolutions(1);
    }

    // But the prefixes must be points.
    TEST_F(QueryPlannerTest, HaveBadPrefixOnTextIndex) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("a" << 1 << "_fts" << "text" << "_ftsx" << 1));
        runQuery(fromjson("{a:{$gt: 1}, $text:{$search: 'blah'}}"));
        assertNumSolutions(0);

        runQuery(fromjson("{$text: {$search: 'blah'}}"));
        assertNumSolutions(0);

        runQuery(fromjson("{$or: [{a:1}, {$text: {$search: 'blah'}}]}"));
        assertNumSolutions(0);
    }

    // There can be more than one prefix, but they all require points.
    TEST_F(QueryPlannerTest, ManyPrefixTextIndex) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("a" << 1 << "b" << 1 << "_fts" << "text" << "_ftsx" << 1));

        // Both points.
        runQuery(fromjson("{a:1, b:1, $text:{$search: 'blah'}}"));
        assertSolutionExists("{text: {prefix: {a:1, b:1}, search: 'blah'}}");
        assertNumSolutions(1);

        // Missing a.
        runQuery(fromjson("{b:1, $text:{$search: 'blah'}}"));
        assertNumSolutions(0);

        // Missing b.
        runQuery(fromjson("{a:1, $text:{$search: 'blah'}}"));
        assertNumSolutions(0);

        // a is not a point
        runQuery(fromjson("{a:{$gt: 1}, b:1, $text:{$search: 'blah'}}"));
        assertNumSolutions(0);

        // b is not a point
        runQuery(fromjson("{a:1, b:{$gt: 1}, $text:{$search: 'blah'}}"));
        assertNumSolutions(0);
    }

    // And, suffixes.  They're optional and don't need to be points.
    TEST_F(QueryPlannerTest, SuffixOptional) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("a" << 1 << "_fts" << "text" << "_ftsx" << 1 << "b" << 1));

        runQuery(fromjson("{a:1, $text:{$search: 'blah'}}"));
        assertNumSolutions(1);
        assertSolutionExists("{text: {prefix: {a:1}, search: 'blah'}}}}");

        runQuery(fromjson("{a:1, b:{$gt: 7}, $text:{$search: 'blah'}}"));
        assertSolutionExists("{text: {prefix: {a:1}, filter: {b: {$gt: 7}}, search: 'blah'}}}}");
        assertNumSolutions(1);
    }

    TEST_F(QueryPlannerTest, RemoveFromSubtree) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("a" << 1 << "_fts" << "text" << "_ftsx" << 1 << "b" << 1));

        runQuery(fromjson("{a:1, $or: [{a:1}, {b:7}], $text:{$search: 'blah'}}"));
        assertNumSolutions(1);

        assertSolutionExists("{fetch: {filter: {$or:[{a:1},{b:7}]},"
                                      "node: {text: {prefix: {a:1}, search: 'blah'}}}}");
    }

    // Text is quite often multikey.  None of the prefixes can be arrays, and suffixes are indexed
    // as-is, so we should compound even if it's multikey.
    TEST_F(QueryPlannerTest, CompoundPrefixEvenIfMultikey) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("a" << 1 << "b" << 1 << "_fts" << "text" << "_ftsx" << 1), true);

        // Both points.
        runQuery(fromjson("{a:1, b:1, $text:{$search: 'blah'}}"));
        assertNumSolutions(1);
        assertSolutionExists("{text: {prefix: {a:1, b:1}, search: 'blah'}}");
    }

    TEST_F(QueryPlannerTest, IndexOnOwnFieldButNotLeafPrefix) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("a" << 1 << "_fts" << "text" << "_ftsx" << 1 << "b" << 1));

        // 'a' is not an EQ so it doesn't compound w/the text pred.  We also shouldn't use the text
        // index to satisfy it w/o the text query.
        runQuery(fromjson("{a:{$elemMatch:{$gt: 0, $lt: 2}}, $text:{$search: 'blah'}}"));
        assertNumSolutions(0);
    }

    TEST_F(QueryPlannerTest, IndexOnOwnFieldButNotLeafSuffixNoPrefix) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("_fts" << "text" << "_ftsx" << 1 << "b" << 1));

        runQuery(fromjson("{b:{$elemMatch:{$gt: 0, $lt: 2}}, $text:{$search: 'blah'}}"));
        assertNumSolutions(1);
    }

}  // namespace

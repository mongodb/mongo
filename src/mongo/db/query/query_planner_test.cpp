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
 * This file contains tests for mongo/db/query/query_planner.cpp
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
    // Equality
    //

    TEST_F(QueryPlannerTest, EqualityIndexScan) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << 5));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, EqualityIndexScanWithTrailingFields) {
        addIndex(BSON("x" << 1 << "y" << 1));

        runQuery(BSON("x" << 5));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1, y: 1}}}}}");
    }

    //
    // <
    //

    TEST_F(QueryPlannerTest, LessThan) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << BSON("$lt" << 5)));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: {$lt: 5}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");
    }

    //
    // <=
    //

    TEST_F(QueryPlannerTest, LessThanEqual) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << BSON("$lte" << 5)));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: {$lte: 5}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
    }

    //
    // >
    //

    TEST_F(QueryPlannerTest, GreaterThan) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << BSON("$gt" << 5)));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: {$gt: 5}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
    }

    //
    // >=
    //

    TEST_F(QueryPlannerTest, GreaterThanEqual) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << BSON("$gte" << 5)));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: {$gte: 5}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
    }

    //
    // Mod
    //

    TEST_F(QueryPlannerTest, Mod) {
        addIndex(BSON("a" << 1));

        runQuery(fromjson("{a: {$mod: [2, 0]}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: {$mod: [2, 0]}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: {a: {$mod: [2, 0]}}, pattern: {a: 1}}}}}");
    }

    //
    // Exists
    //

    TEST_F(QueryPlannerTest, ExistsTrue) {
        addIndex(BSON("x" << 1));

        runQuery(fromjson("{x: 1, y: {$exists: true}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, ExistsFalse) {
        addIndex(BSON("x" << 1));

        runQuery(fromjson("{x: 1, y: {$exists: false}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, ExistsTrueSparseIndex) {
        addIndex(BSON("x" << 1), false, true);

        runQuery(fromjson("{x: 1, y: {$exists: true}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, ExistsFalseSparseIndex) {
        addIndex(BSON("x" << 1), false, true);

        runQuery(fromjson("{x: 1, y: {$exists: false}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    }

    //
    // skip and limit
    //

    TEST_F(QueryPlannerTest, BasicSkipNoIndex) {
        addIndex(BSON("a" << 1));

        runQuerySkipLimit(BSON("x" << 5), 3, 0);

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{skip: {n: 3, node: {cscan: {dir: 1, filter: {x: 5}}}}}");
    }

    TEST_F(QueryPlannerTest, BasicSkipWithIndex) {
        addIndex(BSON("a" << 1 << "b" << 1));

        runQuerySkipLimit(BSON("a" << 5), 8, 0);

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{skip: {n: 8, node: {cscan: {dir: 1, filter: {a: 5}}}}}");
        assertSolutionExists("{skip: {n: 8, node: {fetch: {filter: null, node: "
                                "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}");
    }

    TEST_F(QueryPlannerTest, BasicLimitNoIndex) {
        addIndex(BSON("a" << 1));

        runQuerySkipLimit(BSON("x" << 5), 0, -3);

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{limit: {n: 3, node: {cscan: {dir: 1, filter: {x: 5}}}}}");
    }

    TEST_F(QueryPlannerTest, BasicSoftLimitNoIndex) {
        addIndex(BSON("a" << 1));

        runQuerySkipLimit(BSON("x" << 5), 0, 3);

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
    }

    TEST_F(QueryPlannerTest, BasicLimitWithIndex) {
        addIndex(BSON("a" << 1 << "b" << 1));

        runQuerySkipLimit(BSON("a" << 5), 0, -5);

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{limit: {n: 5, node: {cscan: {dir: 1, filter: {a: 5}}}}}");
        assertSolutionExists("{limit: {n: 5, node: {fetch: {filter: null, node: "
                                "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}");
    }

    TEST_F(QueryPlannerTest, BasicSoftLimitWithIndex) {
        addIndex(BSON("a" << 1 << "b" << 1));

        runQuerySkipLimit(BSON("a" << 5), 0, 5);

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: 5}}}}");
        assertSolutionExists("{fetch: {filter: null, node: "
                                "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, SkipAndLimit) {
        addIndex(BSON("x" << 1));

        runQuerySkipLimit(BSON("x" << BSON("$lte" << 4)), 7, -2);

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{limit: {n: 2, node: {skip: {n: 7, node: "
                                "{cscan: {dir: 1, filter: {x: {$lte: 4}}}}}}}}");
        assertSolutionExists("{limit: {n: 2, node: {skip: {n: 7, node: {fetch: "
                                "{filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}}}}}");
    }

    TEST_F(QueryPlannerTest, SkipAndSoftLimit) {
        addIndex(BSON("x" << 1));

        runQuerySkipLimit(BSON("x" << BSON("$lte" << 4)), 7, 2);

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{skip: {n: 7, node: "
                                "{cscan: {dir: 1, filter: {x: {$lte: 4}}}}}}");
        assertSolutionExists("{skip: {n: 7, node: {fetch: "
                                "{filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}}}");
    }

    //
    // tree operations
    //

    TEST_F(QueryPlannerTest, TwoPredicatesAnding) {
        addIndex(BSON("x" << 1));

        runQuery(fromjson("{$and: [ {x: {$gt: 1}}, {x: {$lt: 3}} ] }"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, SimpleOr) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{a: 20}, {a: 21}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a: 20}, {a: 21}]}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a:1}}}}}");
    }

    TEST_F(QueryPlannerTest, OrWithoutEnoughIndices) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{a: 20}, {b: 21}]}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a: 20}, {b: 21}]}}}");
    }

    TEST_F(QueryPlannerTest, OrWithAndChild) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {or: {nodes: ["
                                "{ixscan: {filter: null, pattern: {a: 1}}}, "
                                "{fetch: {filter: {b: 7}, node: {ixscan: "
                                "{filter: null, pattern: {a: 1}}}}}]}}}}");
    }

    TEST_F(QueryPlannerTest, AndWithUnindexedOrChild) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a:20, $or: [{b:1}, {c:7}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, node: "
                                "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    }


    TEST_F(QueryPlannerTest, AndWithOrWithOneIndex) {
        addIndex(BSON("b" << 1));
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{b:1}, {c:7}], a:20}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    //
    // Min/Max
    //

    TEST_F(QueryPlannerTest, MinValid) {
        addIndex(BSON("a" << 1));
        runQueryHintMinMax(BSONObj(), BSONObj(), fromjson("{a: 1}"), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, MinWithoutIndex) {
        runInvalidQueryHintMinMax(BSONObj(), BSONObj(), fromjson("{a: 1}"), BSONObj());
    }

    TEST_F(QueryPlannerTest, MinBadHint) {
        addIndex(BSON("b" << 1));
        runInvalidQueryHintMinMax(BSONObj(), fromjson("{b: 1}"), fromjson("{a: 1}"), BSONObj());
    }

    TEST_F(QueryPlannerTest, MaxValid) {
        addIndex(BSON("a" << 1));
        runQueryHintMinMax(BSONObj(), BSONObj(), BSONObj(), fromjson("{a: 1}"));

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, MaxWithoutIndex) {
        runInvalidQueryHintMinMax(BSONObj(), BSONObj(), BSONObj(), fromjson("{a: 1}"));
    }

    TEST_F(QueryPlannerTest, MaxBadHint) {
        addIndex(BSON("b" << 1));
        runInvalidQueryHintMinMax(BSONObj(), fromjson("{b: 1}"), BSONObj(), fromjson("{a: 1}"));
    }

    TEST_F(QueryPlannerTest, MaxMinSort) {
        addIndex(BSON("a" << 1));

        // Run an empty query, sort {a: 1}, max/min arguments.
        runQueryFull(BSONObj(), fromjson("{a: 1}"), BSONObj(), 0, 0, BSONObj(),
                     fromjson("{a: 2}"), fromjson("{a: 8}"), false);

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, MaxMinReverseSort) {
        addIndex(BSON("a" << 1));

        // Run an empty query, sort {a: -1}, max/min arguments.
        runQueryFull(BSONObj(), fromjson("{a: -1}"), BSONObj(), 0, 0, BSONObj(),
                     fromjson("{a: 2}"), fromjson("{a: 8}"), false);

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{fetch: {node: {ixscan: {filter: null, dir: -1, pattern: {a: 1}}}}}");
    }


    //
    // $snapshot
    //

    TEST_F(QueryPlannerTest, Snapshot) {
        addIndex(BSON("a" << 1));
        runQuerySnapshot(fromjson("{a: {$gt: 0}}"));

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: {a:{$gt:0}}, node: "
                                "{ixscan: {filter: null, pattern: {_id: 1}}}}}");
    }

    //
    // Tree operations that require simple tree rewriting.
    //

    TEST_F(QueryPlannerTest, AndOfAnd) {
        addIndex(BSON("x" << 1));
        runQuery(fromjson("{$and: [ {$and: [ {x: 2.5}]}, {x: {$gt: 1}}, {x: {$lt: 3}} ] }"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
    }

    //
    // Logically equivalent queries
    //

    TEST_F(QueryPlannerTest, EquivalentAndsOne) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{$and: [{a: 1}, {b: {$all: [10, 20]}}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and:[{a:1},{b:10},{b:20}]}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, EquivalentAndsTwo) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{$and: [{a: 1, b: 10}, {a: 1, b: 20}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and:[{a:1},{a:1},{b:10},{b:20}]}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    //
    // Covering
    //

    TEST_F(QueryPlannerTest, BasicCovering) {
        addIndex(BSON("x" << 1));
        // query, sort, proj
        runQuerySortProj(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{_id: 0, x: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, x: 1}, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, x: 1}, node: "
                                "{cscan: {dir: 1, filter: {x:{$gt:1}}}}}}");
    }

    TEST_F(QueryPlannerTest, DottedFieldCovering) {
        addIndex(BSON("a.b" << 1));
        runQuerySortProj(fromjson("{'a.b': 5}"), BSONObj(), fromjson("{_id: 0, 'a.b': 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, 'a.b': 1}, node: "
                                "{cscan: {dir: 1, filter: {'a.b': 5}}}}}");
        // SERVER-2104
        //assertSolutionExists("{proj: {spec: {_id: 0, 'a.b': 1}, node: {'a.b': 1}}}");
    }

    TEST_F(QueryPlannerTest, IdCovering) {
        runQuerySortProj(fromjson("{_id: {$gt: 10}}"), BSONObj(), fromjson("{_id: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 1}, node: "
                                "{cscan: {dir: 1, filter: {_id: {$gt: 10}}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 1}, node: {ixscan: "
                                "{filter: null, pattern: {_id: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, ProjNonCovering) {
        addIndex(BSON("x" << 1));
        runQuerySortProj(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{x: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {x: 1}, node: {cscan: "
                                "{dir: 1, filter: {x: {$gt: 1}}}}}}");
        assertSolutionExists("{proj: {spec: {x: 1}, node: {fetch: {filter: null, node: "
                                "{ixscan: {filter: null, pattern: {x: 1}}}}}}}");
    }

    //
    // Basic sort
    //

    TEST_F(QueryPlannerTest, BasicSort) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        runQuerySortProj(fromjson("{ a : 5 }"), BSON("b" << 1), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{sort: {pattern: {b: 1}, limit: 0, "
                                "node: {cscan: {dir: 1, filter: {a: 5}}}}}");
        assertSolutionExists("{sort: {pattern: {b: 1}, limit: 0, "
                                "node: {fetch: {filter: null, node: "
                                "{ixscan: {filter: null, pattern: {a: 1}}}}}}}");
        assertSolutionExists("{fetch: {filter: {a: 5}, node: {ixscan: "
                                "{filter: null, pattern: {b: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, BasicSortBooleanIndexKeyPattern) {
        addIndex(BSON("a" << true));
        runQuerySortProj(fromjson("{ a : 5 }"), BSON("a" << 1), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 0, "
                                "node: {cscan: {dir: 1, filter: {a: 5}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: true}}}}}");
    }

    //
    // Sort with limit and/or skip
    //

    TEST_F(QueryPlannerTest, SortLimit) {
        // Negative limit indicates hard limit - see lite_parsed_query.cpp
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 0, -3);
        assertNumSolutions(1U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 3, "
                                "node: {cscan: {dir: 1}}}}");
    }

    TEST_F(QueryPlannerTest, SortSkip) {
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, 0);
        assertNumSolutions(1U);
        // If only skip is provided, do not limit sort.
        assertSolutionExists("{skip: {n: 2, node: "
                                "{sort: {pattern: {a: 1}, limit: 0, "
                                "node: {cscan: {dir: 1}}}}}}");
    }

    TEST_F(QueryPlannerTest, SortSkipLimit) {
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, -3);
        assertNumSolutions(1U);
        // Limit in sort node should be adjusted by skip count
        assertSolutionExists("{skip: {n: 2, node: "
                                "{sort: {pattern: {a: 1}, limit: 5, "
                                "node: {cscan: {dir: 1}}}}}}");
    }

    TEST_F(QueryPlannerTest, SortSoftLimit) {
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 0, 3);
        assertNumSolutions(1U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 3, "
                                "node: {cscan: {dir: 1}}}}");
    }

    TEST_F(QueryPlannerTest, SortSkipSoftLimit) {
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, 3);
        assertNumSolutions(1U);
        assertSolutionExists("{skip: {n: 2, node: "
                                "{sort: {pattern: {a: 1}, limit: 5, "
                                "node: {cscan: {dir: 1}}}}}}");
    }

    //
    // Basic sort elimination
    //

    TEST_F(QueryPlannerTest, BasicSortElim) {
        addIndex(BSON("x" << 1));
        // query, sort, proj
        runQuerySortProj(fromjson("{ x : {$gt: 1}}"), fromjson("{x: 1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {x: 1}, limit: 0, "
                                "node: {cscan: {dir: 1, filter: {x: {$gt: 1}}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {x: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, SortElimCompound) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProj(fromjson("{ a : 5 }"), BSON("b" << 1), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {b: 1}, limit: 0, "
                                "node: {cscan: {dir: 1, filter: {a: 5}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    //
    // Basic compound
    //

    TEST_F(QueryPlannerTest, BasicCompound) {
        addIndex(BSON("x" << 1 << "y" << 1));
        runQuery(fromjson("{ x : 5, y: 10}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1, y: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, CompoundMissingField) {
        addIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
        runQuery(fromjson("{ x : 5, z: 10}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: {z: 10}, node: "
                                "{ixscan: {filter: null, pattern: {x: 1, y: 1, z: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, CompoundFieldsOrder) {
        addIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
        runQuery(fromjson("{ x : 5, z: 10, y:1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1, y: 1, z: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, CantUseCompound) {
        addIndex(BSON("x" << 1 << "y" << 1));
        runQuery(fromjson("{ y: 10}"));

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{cscan: {dir: 1, filter: {y: 10}}}");
    }

    //
    // Array operators
    //

    TEST_F(QueryPlannerTest, ElemMatchOneField) {
        addIndex(BSON("a.b" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1}}}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a:{$elemMatch:{b:1}}}}}");
        assertSolutionExists("{fetch: {filter: {a:{$elemMatch:{b:1}}}, node: "
                                "{ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
    }

    TEST_F(QueryPlannerTest, ElemMatchTwoFields) {
        addIndex(BSON("a.b" << 1));
        addIndex(BSON("a.c" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));

        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a:{$elemMatch:{b:1,c:1}}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {'a.c': 1}}}}}");
    }

    TEST_F(QueryPlannerTest, BasicAllElemMatch) {
        addIndex(BSON("foo.a" << 1));
        addIndex(BSON("foo.b" << 1));
        runQuery(fromjson("{foo: {$all: [ {$elemMatch: {a:1, b:1}}, {$elemMatch: {a:2, b:2}}]}}"));

        ASSERT_EQUALS(getNumSolutions(), 5U);
        assertSolutionExists("{cscan: {dir: 1, filter: {foo:{$all:"
                                "[{$elemMatch:{a:1,b:1}},{$elemMatch:{a:2,b:2}}]}}}}");
        // Two solutions exist for 'foo.a' which differ only by assignment of index tags.
        assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {'foo.a': 1}}}}}", 2);
        // Two solutions exist for 'foo.b' which differ only by assignment of index tags.
        assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {'foo.b': 1}}}}}", 2);

        // TODO: We're not checking for the filter above because canonically sorting
        // the filter means that we cannot distinguish between match expressions that
        // are tagged differently. We may want to add this capability to the query
        // planner test lib in the future.
        /*assertSolutionExists("{fetch: {filter: {foo:{$all:[{$elemMatch:{a:1,b:1}},{$elemMatch:{a:2,b:2}}]}}, "
                                "node: {ixscan: {filter: null, pattern: {'foo.a': 1}}}}}");
        assertSolutionExists("{fetch: {filter: {foo:{$all:[{$elemMatch:{a:2,b:2}},{$elemMatch:{a:1,b:1}}]}}, "
                                "node: {ixscan: {filter: null, pattern: {'foo.a': 1}}}}}");
        assertSolutionExists("{fetch: {filter: {foo:{$all:[{$elemMatch:{b:1,a:1}},{$elemMatch:{a:2,b:2}}]}}, "
                                "node: {ixscan: {filter: null, pattern: {'foo.b': 1}}}}}");
        assertSolutionExists("{fetch: {filter: {foo:{$all:[{$elemMatch:{b:2,a:2}},{$elemMatch:{a:1,b:1}}]}}, "
                                "node: {ixscan: {filter: null, pattern: {'foo.b': 1}}}}}");*/
    }

    TEST_F(QueryPlannerTest, ElemMatchValueMatch) {
        addIndex(BSON("foo" << 1));
        addIndex(BSON("foo" << 1 << "bar" << 1));
        runQuery(fromjson("{foo: {$elemMatch: {$gt: 5, $lt: 10}}}"));

        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1, filter: {foo:{$elemMatch:{$gt:5,$lt:10}}}}}");
        assertSolutionExists("{fetch: {filter: {foo: {$elemMatch: {$gt: 5, $lt: 10}}}, node: "
                                "{ixscan: {filter: null, pattern: {foo: 1}}}}}");
        assertSolutionExists("{fetch: {filter: {foo: {$elemMatch: {$gt: 5, $lt: 10}}}, node: "
                                "{ixscan: {filter: null, pattern: {foo: 1, bar: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, ElemMatchNested) {
        addIndex(BSON("a.b.c" << 1));
        runQuery(fromjson("{ a:{ $elemMatch:{ b:{ $elemMatch:{ c:{ $gte:1, $lte:1 } } } } }}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}}}}}");
    }

    TEST_F(QueryPlannerTest, TwoElemMatchNested) {
        addIndex(BSON("a.d.e" << 1));
        addIndex(BSON("a.b.c" << 1));
        runQuery(fromjson("{ a:{ $elemMatch:{ d:{ $elemMatch:{ e:{ $lte:1 } } },"
                             "b:{ $elemMatch:{ c:{ $gte:1 } } } } } }"));

        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.d.e': 1}}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}}}}}");
    }

    TEST_F(QueryPlannerTest, ElemMatchCompoundTwoFields) {
        addIndex(BSON("a.b" << 1 << "a.c" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}}}}}");
    }

    TEST_F(QueryPlannerTest, ArrayEquality) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a : [1, 2, 3]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a:[1,2,3]}}}");
        assertSolutionExists("{fetch: {filter: {a:[1,2,3]}, node: "
                                "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    // $not can appear as a value operator inside of an elemMatch (value).  We shouldn't crash if we
    // see it.
    TEST_F(QueryPlannerTest, ElemMatchWithNotInside) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a: {$elemMatch: {$not: {$gte: 6}}}}"));
    }

    //
    // Geo
    // http://docs.mongodb.org/manual/reference/operator/query-geospatial/#geospatial-query-compatibility-chart
    //

    TEST_F(QueryPlannerTest, Basic2DNonNear) {
        // 2d can answer: within poly, within center, within centersphere, within box.
        // And it can use an index (or not) for each of them.  As such, 2 solns expected.
        addIndex(BSON("a" << "2d"));

        // Polygon
        runQuery(fromjson("{a : { $within: { $polygon : [[0,0], [2,0], [4,0]] } }}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {geo2d: {a: '2d'}}}}");

        // Center
        runQuery(fromjson("{a : { $within : { $center : [[ 5, 5 ], 7 ] } }}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {geo2d: {a: '2d'}}}}");

        // Centersphere
        runQuery(fromjson("{a : { $within : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {geo2d: {a: '2d'}}}}");

        // Within box.
        runQuery(fromjson("{a : {$within: {$box : [[0,0],[9,9]]}}}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {geo2d: {a: '2d'}}}}");

        // TODO: test that we *don't* annotate for things we shouldn't.
    }

    TEST_F(QueryPlannerTest, Basic2DSphereNonNear) {
        // 2dsphere can do: within+geometry, intersects+geometry
        addIndex(BSON("a" << "2dsphere"));

        runQuery(fromjson("{a: {$geoIntersects: {$geometry: {type: 'Point',"
                                                           "coordinates: [10.0, 10.0]}}}}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");

        runQuery(fromjson("{a : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");

        // TODO: test that we *don't* annotate for things we shouldn't.
    }

    TEST_F(QueryPlannerTest, Basic2DGeoNear) {
        // Can only do near + old point.
        addIndex(BSON("a" << "2d"));
        runQuery(fromjson("{a: {$near: [0,0], $maxDistance:0.3 }}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{geoNear2d: {a: '2d'}}");
    }

    TEST_F(QueryPlannerTest, Basic2DSphereGeoNear) {
        // Can do nearSphere + old point, near + new point.
        addIndex(BSON("a" << "2dsphere"));

        runQuery(fromjson("{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{geoNear2dsphere: {a: '2dsphere'}}");

        runQuery(fromjson("{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
                                          "$maxDistance:100}}}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{geoNear2dsphere: {a: '2dsphere'}}");
    }

    TEST_F(QueryPlannerTest, Basic2DSphereGeoNearReverseCompound) {
        addIndex(BSON("x" << 1));
        addIndex(BSON("x" << 1 << "a" << "2dsphere"));
        runQuery(fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{geoNear2dsphere: {x: 1, a: '2dsphere'}}");
    }

    TEST_F(QueryPlannerTest, NearNoIndex) {
        addIndex(BSON("x" << 1));
        runInvalidQuery(fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));
    }

    TEST_F(QueryPlannerTest, TwoDSphereNoGeoPred) {
        addIndex(BSON("x" << 1 << "a" << "2dsphere"));
        runQuery(fromjson("{x:1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1, a: '2dsphere'}}}}}");
    }

    // SERVER-3984, $or 2d index
    TEST_F(QueryPlannerTest, Or2DNonNear) {
        addIndex(BSON("a" << "2d"));
        addIndex(BSON("b" << "2d"));
        runQuery(fromjson("{$or: [ {a : { $within : { $polygon : [[0,0], [2,0], [4,0]] } }},"
                                 " {b : { $within : { $center : [[ 5, 5 ], 7 ] } }} ]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {or: {nodes: [{geo2d: {a: '2d'}}, {geo2d: {b: '2d'}}]}}}}");
    }

    // SERVER-3984, $or 2dsphere index
    TEST_F(QueryPlannerTest, Or2DSphereNonNear) {
        addIndex(BSON("a" << "2dsphere"));
        addIndex(BSON("b" << "2dsphere"));
        runQuery(fromjson("{$or: [ {a: {$geoIntersects: {$geometry: {type: 'Point', coordinates: [10.0, 10.0]}}}},"
                                 " {b: {$geoWithin: { $centerSphere: [[ 10, 20 ], 0.01 ] } }} ]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{or: {nodes: [{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}},"
                                           "{fetch: {node: {ixscan: {pattern: {b: '2dsphere'}}}}}]}}");
    }

    //
    // $in
    //

    TEST_F(QueryPlannerTest, InBasic) {
        addIndex(fromjson("{a: 1}"));
        runQuery(fromjson("{a: {$in: [1, 2]}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: {$in: [1, 2]}}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1}}}}}");
    }

    // Logically equivalent to the preceding $in query.
    // Indexed solution should be the same.
    TEST_F(QueryPlannerTest, InBasicOrEquivalent) {
        addIndex(fromjson("{a: 1}"));
        runQuery(fromjson("{$or: [{a: 1}, {a: 2}]}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a: 1}, {a: 2}]}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, InCompoundIndexFirst) {
        addIndex(fromjson("{a: 1, b: 1}"));
        runQuery(fromjson("{a: {$in: [1, 2]}, b: 3}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {b: 3, a: {$in: [1, 2]}}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
    }

    // Logically equivalent to the preceding $in query.
    // Indexed solution should be the same.
    // Currently fails - pre-requisite to SERVER-12024
    /*
    TEST_F(QueryPlannerTest, InCompoundIndexFirstOrEquivalent) {
        addIndex(fromjson("{a: 1, b: 1}"));
        runQuery(fromjson("{$and: [{$or: [{a: 1}, {a: 2}]}, {b: 3}]}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{$or: [{a: 1}, {a: 2}]}, {b: 3}]}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
    }
    */

    TEST_F(QueryPlannerTest, InCompoundIndexLast) {
        addIndex(fromjson("{a: 1, b: 1}"));
        runQuery(fromjson("{a: 3, b: {$in: [1, 2]}}"));

        assertNumSolutions(2U);
        // TODO: update filter in cscan solution when SERVER-12024 is implemented
        assertSolutionExists("{cscan: {dir: 1, filter: {a: 3, b: {$in: [1, 2]}}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
    }

    // Logically equivalent to the preceding $in query.
    // Indexed solution should be the same.
    // Currently fails - pre-requisite to SERVER-12024
    /*
    TEST_F(QueryPlannerTest, InCompoundIndexLastOrEquivalent) {
        addIndex(fromjson("{a: 1, b: 1}"));
        runQuery(fromjson("{$and: [{a: 3}, {$or: [{b: 1}, {b: 2}]}]}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{a: 3}, {$or: [{b: 1}, {b: 2}]}]}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
    }
    */

    // SERVER-1205
    TEST_F(QueryPlannerTest, InWithSort) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProjSkipLimit(fromjson("{a: {$in: [1, 2]}}"),
                                  BSON("b" << 1), BSONObj(), 0, 1);

        assertSolutionExists("{sort: {pattern: {b: 1}, limit: 1, "
                             "node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {node: {mergeSort: {nodes: "
                                "[{ixscan: {pattern: {a: 1, b: 1}}}, {ixscan: {pattern: {a: 1, b: 1}}}]}}}}");
    }

    // SERVER-1205
    TEST_F(QueryPlannerTest, InWithoutSort) {
        addIndex(BSON("a" << 1 << "b" << 1));
        // No sort means we don't bother to blow up the bounds.
        runQuerySortProjSkipLimit(fromjson("{a: {$in: [1, 2]}}"), BSONObj(), BSONObj(), 0, 1);

        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
    }

    // SERVER-1205
    TEST_F(QueryPlannerTest, ManyInWithSort) {
        addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1));
        runQuerySortProjSkipLimit(fromjson("{a: {$in: [1, 2]}, b:{$in:[1,2]}, c:{$in:[1,2]}}"),
                                  BSON("d" << 1), BSONObj(), 0, 1);

        assertSolutionExists("{sort: {pattern: {d: 1}, limit: 1, "
                             "node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {node: {mergeSort: {nodes: "
                                "[{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
                                 "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
                                 "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
                                 "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
                                 "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
                                  "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}}]}}}}");
    }

    // SERVER-1205
    TEST_F(QueryPlannerTest, TooManyToExplode) {
        addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1));
        runQuerySortProjSkipLimit(fromjson("{a: {$in: [1,2,3,4,5,6]},"
                                            "b:{$in:[1,2,3,4,5,6,7,8]},"
                                            "c:{$in:[1,2,3,4,5,6,7,8]}}"),
                                  BSON("d" << 1), BSONObj(), 0, 1);

        // We cap the # of ixscans we're willing to create.
        assertNumSolutions(2);
        assertSolutionExists("{sort: {pattern: {d: 1}, limit: 1, "
                             "node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{sort: {pattern: {d: 1}, limit: 1, node: "
                             "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}}}}}}");
    }

    //
    // Multiple solutions
    //

    TEST_F(QueryPlannerTest, TwoPlans) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("a" << 1 << "b" << 1));

        runQuery(fromjson("{a:1, b:{$gt:2,$lt:2}}"));

        // 2 indexed solns and one non-indexed
        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and:[{b:{$lt:2}},{a:1},{b:{$gt:2}}]}}}");
        assertSolutionExists("{fetch: {filter: {$and:[{b:{$lt:2}},{b:{$gt:2}}]}, node: "
                                "{ixscan: {filter: null, pattern: {a: 1}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, TwoPlansElemMatch) {
        addIndex(BSON("a" << 1 << "b" << 1));
        addIndex(BSON("arr.x" << 1 << "a" << 1));

        runQuery(fromjson("{arr: { $elemMatch : { x : 5 , y : 5 } },"
                          " a : 55 , b : { $in : [ 1 , 5 , 8 ] } }"));

        // 2 indexed solns and one non-indexed
        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
        assertSolutionExists("{fetch: {filter: {$and: [{a: 55}, {b: {$in: [1, 5, 8]}}]}, "
                                      "node: {fetch: {filter: {arr: {$elemMatch: {x: 5, y: 5}}}, "
                                            "node: {ixscan: {pattern: {'arr.x': 1, a: 1}}}}}}}");
    }

    TEST_F(QueryPlannerTest, CompoundAndNonCompoundIndices) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("a" << 1 << "b" << 1), true);
        runQuery(fromjson("{a: 1, b: {$gt: 2, $lt: 2}}"));

        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: {$and:[{b:{$lt:2}},{b:{$gt:2}}]}, node: "
                                "{ixscan: {pattern: {a:1}, bounds: {a: [[1,1,true,true]]}}}}}");
        assertSolutionExists("{fetch: {filter: {$and:[{b:{$lt:2}},{b:{$gt:2}}]}, node: "
                                "{ixscan: {pattern: {a:1,b:1}, bounds: "
                                "{a: [[1,1,true,true]], b: [['MinKey','MaxKey',true,true]]}}}}}");
    }

    //
    // Sort orders
    //

    // SERVER-1205.
    TEST_F(QueryPlannerTest, MergeSort) {
        addIndex(BSON("a" << 1 << "c" << 1));
        addIndex(BSON("b" << 1 << "c" << 1));
        runQuerySortProj(fromjson("{$or: [{a:1}, {b:1}]}"), fromjson("{c:1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {c: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {node: {mergeSort: {nodes: "
                                "[{ixscan: {pattern: {a: 1, c: 1}}}, {ixscan: {pattern: {b: 1, c: 1}}}]}}}}");
    }

    // SERVER-1205 as well.
    TEST_F(QueryPlannerTest, NoMergeSortIfNoSortWanted) {
        addIndex(BSON("a" << 1 << "c" << 1));
        addIndex(BSON("b" << 1 << "c" << 1));
        runQuerySortProj(fromjson("{$or: [{a:1}, {b:1}]}"), BSONObj(), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a:1}, {b:1}]}}}");
        assertSolutionExists("{fetch: {filter: null, node: {or: {nodes: ["
                                "{ixscan: {filter: null, pattern: {a: 1, c: 1}}}, "
                                "{ixscan: {filter: null, pattern: {b: 1, c: 1}}}]}}}}");
    }

    // SERVER-10801
    TEST_F(QueryPlannerTest, SortOnGeoQuery) {
        addIndex(BSON("timestamp" << -1 << "position" << "2dsphere"));
        BSONObj query = fromjson("{position: {$geoWithin: {$geometry: {type: \"Polygon\", coordinates: [[[1, 1], [1, 90], [180, 90], [180, 1], [1, 1]]]}}}}");
        BSONObj sort = fromjson("{timestamp: -1}");
        runQuerySortProj(query, sort, BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {timestamp: -1}, limit: 0, "
                                "node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {timestamp: -1, position: '2dsphere'}}}}}");
    }

    // SERVER-9257
    TEST_F(QueryPlannerTest, CompoundGeoNoGeoPredicate) {
        addIndex(BSON("creationDate" << 1 << "foo.bar" << "2dsphere"));
        runQuerySortProj(fromjson("{creationDate: { $gt: 7}}"),
                         fromjson("{creationDate: 1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {creationDate: 1}, limit: 0, "
                                "node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {creationDate: 1, 'foo.bar': '2dsphere'}}}}}");
    }

    // Basic "keep sort in mind with an OR"
    TEST_F(QueryPlannerTest, MergeSortEvenIfSameIndex) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProj(fromjson("{$or: [{a:1}, {a:7}]}"), fromjson("{b:1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {b: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
        // TODO the second solution should be mergeSort rather than just sort
    }

    TEST_F(QueryPlannerTest, ReverseScanForSort) {
        addIndex(BSON("_id" << 1));
        runQuerySortProj(BSONObj(), fromjson("{_id: -1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {_id: -1}, limit: 0, node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {_id: 1}}}}}");
    }

    //
    // Hint tests
    //

    TEST_F(QueryPlannerTest, NaturalHint) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        runQuerySortHint(BSON("a" << 1), BSON("b" << 1), BSON("$natural" << 1));

        assertNumSolutions(1U);
        assertSolutionExists("{sort: {pattern: {b: 1}, limit: 0, node: "
                                "{cscan: {filter: {a: 1}, dir: 1}}}}");
    }

    TEST_F(QueryPlannerTest, HintValid) {
        addIndex(BSON("a" << 1));
        runQueryHint(BSONObj(), fromjson("{a: 1}"));

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, HintValidWithPredicate) {
        addIndex(BSON("a" << 1));
        runQueryHint(fromjson("{a: {$gt: 1}}"), fromjson("{a: 1}"));

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, HintValidWithSort) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        runQuerySortHint(fromjson("{a: 100, b: 200}"), fromjson("{b: 1}"), fromjson("{a: 1}"));

        assertNumSolutions(1U);
        assertSolutionExists("{sort: {pattern: {b: 1}, limit: 0, node: "
                                "{fetch: {filter: {b: 200}, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}}}");
    }

    TEST_F(QueryPlannerTest, HintMultipleSolutions) {
        addIndex(fromjson("{'a.b': 1}"));
        runQueryHint(fromjson("{'a.b': 1, a: {$elemMatch: {b: 2}}}"), fromjson("{'a.b': 1}"));

        assertNumSolutions(2U);
        assertSolutionExists("{fetch: {filter: {a: {$elemMatch: {b: 2}}}, "
                                "node: {ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
        assertSolutionExists("{fetch: {filter: {'a.b': 1},"
                                "node: {fetch: {filter: {a: {$elemMatch: {b: 2}}}, "
                                "node: {ixscan: {filter: null, pattern: {'a.b': 1}}}}}}}");
    }

    TEST_F(QueryPlannerTest, HintInvalid) {
        addIndex(BSON("a" << 1));
        runInvalidQueryHint(BSONObj(), fromjson("{b: 1}"));
    }

    //
    // Sparse indices, SERVER-8067
    // Each index in this block of tests is sparse.
    //

    TEST_F(QueryPlannerTest, SparseIndexIgnoreForSort) {
        addIndex(fromjson("{a: 1}"), false, true);
        runQuerySortProj(BSONObj(), fromjson("{a: 1}"), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
    }

    TEST_F(QueryPlannerTest, SparseIndexHintForSort) {
        addIndex(fromjson("{a: 1}"), false, true);
        runQuerySortHint(BSONObj(), fromjson("{a: 1}"), fromjson("{a: 1}"));

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, SparseIndexPreferCompoundIndexForSort) {
        addIndex(fromjson("{a: 1}"), false, true);
        addIndex(fromjson("{a: 1, b: 1}"));
        runQuerySortProj(BSONObj(), fromjson("{a: 1}"), BSONObj());

        assertNumSolutions(2U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, SparseIndexForQuery) {
        addIndex(fromjson("{a: 1}"), false, true);
        runQuerySortProj(fromjson("{a: 1}"), BSONObj(), BSONObj());

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: 1}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1}}}}}");
    }

    //
    // Regex
    //

    TEST_F(QueryPlannerTest, PrefixRegex) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a: /^foo/}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: /^foo/}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, PrefixRegexCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: /^foo/}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {a: /^foo/}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NonPrefixRegex) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a: /foo/}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: /foo/}}}");
        assertSolutionExists("{fetch: {filter: null, node: "
                                "{ixscan: {filter: {a: /foo/}, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NonPrefixRegexCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: /foo/}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {a: /foo/}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {a: /foo/}, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NonPrefixRegexAnd) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{a: /foo/, b: 2}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{b: 2}, {a: /foo/}]}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: {a: /foo/}, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NonPrefixRegexAndCovering) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProj(fromjson("{a: /foo/, b: 2}"), BSONObj(),
                         fromjson("{_id: 0, a: 1, b: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
                                "{cscan: {dir: 1, filter: {$and: [{b: 2}, {a: /foo/}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
                                "{ixscan: {filter: {a: /foo/}, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NonPrefixRegexOrCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$or: [{a: /0/}, {a: /1/}]}"), BSONObj(),
                         fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {$or: [{a: /0/}, {a: /1/}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {$or: [{a: /0/}, {a: /1/}]}, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NonPrefixRegexInCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: {$in: [/foo/, /bar/]}}"), BSONObj(),
                         fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {a:{$in:[/foo/,/bar/]}}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {a:{$in:[/foo/,/bar/]}}, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, TwoRegexCompoundIndexCovering) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProj(fromjson("{a: /0/, b: /1/}"), BSONObj(),
                         fromjson("{_id: 0, a: 1, b: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
                                "{cscan: {dir: 1, filter: {$and:[{a:/0/},{b:/1/}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
                                "{ixscan: {filter: {$and:[{a:/0/},{b:/1/}]}, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, TwoRegexSameFieldCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$and: [{a: /0/}, {a: /1/}]}"), BSONObj(),
                         fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {$and:[{a:/0/},{a:/1/}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {$and:[{a:/0/},{a:/1/}]}, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, ThreeRegexSameFieldCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$and: [{a: /0/}, {a: /1/}, {a: /2/}]}"), BSONObj(),
                         fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}, pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NonPrefixRegexMultikey) {
        // true means multikey
        addIndex(BSON("a" << 1), true);
        runQuery(fromjson("{a: /foo/}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {filter: {a: /foo/}, dir: 1}}");
        assertSolutionExists("{fetch: {filter: {a: /foo/}, node: {ixscan: "
                                "{pattern: {a: 1}, filter: null}}}}");
    }

    TEST_F(QueryPlannerTest, ThreeRegexSameFieldMultikey) {
        // true means multikey
        addIndex(BSON("a" << 1), true);
        runQuery(fromjson("{$and: [{a: /0/}, {a: /1/}, {a: /2/}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}, dir: 1}}");
        assertSolutionExists("{fetch: {filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}, node: {ixscan: "
                                "{pattern: {a: 1}, filter: null}}}}");
    }

    //
    // Negation
    //

    TEST_F(QueryPlannerTest, NegationIndexForSort) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: {$ne: 1}}"), fromjson("{a: 1}"), BSONObj());

        assertNumSolutions(2U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NegationTopLevel) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: {$ne: 1}}"), BSONObj(), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{cscan: {dir: 1}}");
    }

    TEST_F(QueryPlannerTest, NegationOr) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$or: [{a: 1}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{cscan: {dir: 1}}");
    }

    TEST_F(QueryPlannerTest, NegationOrNotIn) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$or: [{a: 1}, {b: {$nin: [1]}}]}"), BSONObj(), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{cscan: {dir: 1}}");
    }

    TEST_F(QueryPlannerTest, NegationAndIndexOnEquality) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$and: [{a: 1}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NegationAndIndexOnEqualityAndNegationBranches) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        runQuerySortProj(fromjson("{$and: [{a: 1}, {b: 2}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

        assertNumSolutions(3U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {b: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NegationAndIndexOnInEquality) {
        addIndex(BSON("b" << 1));
        runQuerySortProj(fromjson("{$and: [{a: 1}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{cscan: {dir: 1}}");
    }

    //
    // 2D geo negation
    // The filter b != 1 is embedded in the geoNear2d node.
    // Can only do near + old point.
    //
    TEST_F(QueryPlannerTest, Negation2DGeoNear) {
        addIndex(BSON("a" << "2d"));
        runQuery(fromjson("{$and: [{a: {$near: [0, 0], $maxDistance: 0.3}}, {b: {$ne: 1}}]}"));
        assertNumSolutions(1U);
        assertSolutionExists("{geoNear2d: {a: '2d'}}");
    }

    //
    // 2DSphere geo negation
    // Filter is embedded in a separate fetch node.
    //
    TEST_F(QueryPlannerTest, Negation2DSphereGeoNear) {
        // Can do nearSphere + old point, near + new point.
        addIndex(BSON("a" << "2dsphere"));

        runQuery(fromjson("{$and: [{a: {$nearSphere: [0,0], $maxDistance: 0.31}}, "
                          "{b: {$ne: 1}}]}"));
        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {node: {geoNear2dsphere: {a: '2dsphere'}}}}");

        runQuery(fromjson("{$and: [{a: {$geoNear: {$geometry: {type: 'Point', "
                                                              "coordinates: [0, 0]},"
                                                  "$maxDistance: 100}}},"
                                  "{b: {$ne: 1}}]}"));
        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {node: {geoNear2dsphere: {a: '2dsphere'}}}}");
    }

    //
    // Multikey indices
    //

    /**
     * Index bounds constraints on a field should not be intersected
     * if the index is multikey.
     */
    TEST_F(QueryPlannerTest, MultikeyTwoConstraintsSameField) {
        addIndex(BSON("a" << 1), true);
        runQuery(fromjson("{a: {$gt: 0, $lt: 5}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {filter: {$and: [{a: {$lt: 5}}, {a: {$gt: 0}}]}, dir: 1}}");

        vector<string> alternates;
        alternates.push_back("{fetch: {filter: {a: {$lt: 5}}, node: {ixscan: {filter: null, "
                                "pattern: {a: 1}, bounds: {a: [[0, Infinity, false, true]]}}}}}");
        alternates.push_back("{fetch: {filter: {a: {$gt: 0}}, node: {ixscan: {filter: null, "
                                "pattern: {a: 1}, bounds: {a: [[-Infinity, 5, true, false]]}}}}}");
        assertHasOneSolutionOf(alternates);
    }

    /**
     * Constraints on fields with a shared parent should not be intersected
     * if the index is multikey.
     */
    TEST_F(QueryPlannerTest, MultikeyTwoConstraintsDifferentFields) {
        addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
        runQuery(fromjson("{'a.b': 2, 'a.c': 3}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {filter: {$and: [{'a.b': 2}, {'a.c': 3}]}, dir: 1}}");

        vector<string> alternates;
        alternates.push_back("{fetch: {filter: {'a.c': 3}, node: {ixscan: {filter: null, "
                                "pattern: {'a.b': 1, 'a.c': 1}, bounds: "
                                "{'a.b': [[2,2,true,true]], "
                                " 'a.c': [['MinKey','MaxKey',true,true]]}}}}}");
        alternates.push_back("{fetch: {filter: {'a.b': 2}, node: {ixscan: {filter: null, "
                                "pattern: {'a.b': 1, 'a.c': 1}, bounds: "
                                "{'a.b': [['MinKey','MaxKey',true,true]], "
                                " 'a.c': [[3,3,true,true]]}}}}}");
        assertHasOneSolutionOf(alternates);
    }

    //
    // Index bounds related tests
    //

    TEST_F(QueryPlannerTest, CompoundIndexBoundsLastFieldMissing) {
        addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
        runQuery(fromjson("{a: 5, b: {$gt: 7}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: "
                                "{a: [[5,5,true,true]], b: [[7,Infinity,false,true]], "
                                " c: [['MinKey','MaxKey',true,true]]}}}}}");
    }

    TEST_F(QueryPlannerTest, CompoundIndexBoundsMiddleFieldMissing) {
        addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
        runQuery(fromjson("{a: 1, c: {$lt: 3}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: "
                                "{a: [[1,1,true,true]], b: [['MinKey','MaxKey',true,true]], "
                                " c: [['MinKey','MaxKey',true,true]]}}}}}");
    }

    TEST_F(QueryPlannerTest, CompoundIndexBoundsRangeAndEquality) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{a: {$gt: 8}, b: 6}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, bounds: "
                                "{a: [[8,Infinity,false,true]], b:[[6,6,true,true]]}}}}}");
    }

    TEST_F(QueryPlannerTest, CompoundIndexBoundsEqualityThenIn) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{a: 5, b: {$in: [2,6,11]}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {filter: null, pattern: "
                                "{a: 1, b: 1}, bounds: {a: [[5,5,true,true]], "
                                 "b:[[2,2,true,true],[6,6,true,true],[11,11,true,true]]}}}}}");
    }

    TEST_F(QueryPlannerTest, CompoundIndexBoundsStringBounds) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{a: {$gt: 'foo'}, b: {$gte: 'bar'}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {filter: null, pattern: "
                                "{a: 1, b: 1}, bounds: {a: [['foo',{},false,false]], "
                                 "b:[['bar',{},true,false]]}}}}}");
    }

    TEST_F(QueryPlannerTest, IndexBoundsAndWithNestedOr) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$and: [{a: 1, $or: [{a: 2}, {a: 3}]}]}"));

        assertNumSolutions(3U);
        assertSolutionExists("{cscan: {dir: 1, filter: "
                                "{$and: [{$or: [{a: 2}, {a: 3}]}, {a: 1}]}}}");
        assertSolutionExists("{fetch: {filter: {a: 1}, node: {ixscan: "
                                "{pattern: {a: 1}, filter: null,"
                                " bounds: {a: [[2,2,true,true], [3,3,true,true]]}}}}}");
        assertSolutionExists("{fetch: {filter: {$or: [{a: 2}, {a: 3}]}, node: {ixscan: "
                                "{pattern: {a: 1}, filter: null, bounds: {a: [[1,1,true,true]]}}}}}");
    }

    TEST_F(QueryPlannerTest, IndexBoundsIndexedSort) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$or: [{a: 1}, {a: 2}]}"), BSON("a" << 1), BSONObj());

        assertNumSolutions(2U);
        assertSolutionExists("{sort: {pattern: {a:1}, limit: 0, node: "
                                "{cscan: {filter: {$or:[{a:1},{a:2}]}, dir: 1}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {filter: null, "
                                "pattern: {a:1}, bounds: {a: [[1,1,true,true], [2,2,true,true]]}}}}}");
    }

    TEST_F(QueryPlannerTest, IndexBoundsUnindexedSort) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$or: [{a: 1}, {a: 2}]}"), BSON("b" << 1), BSONObj());

        assertNumSolutions(2U);
        assertSolutionExists("{sort: {pattern: {b:1}, limit: 0, node: "
                                "{cscan: {filter: {$or:[{a:1},{a:2}]}, dir: 1}}}}");
        assertSolutionExists("{sort: {pattern: {b:1}, limit: 0, node: {fetch: "
                                "{filter: null, node: {ixscan: {filter: null, "
                                "pattern: {a:1}, bounds: {a: [[1,1,true,true], [2,2,true,true]]}}}}}}}");
    }

    TEST_F(QueryPlannerTest, IndexBoundsUnindexedSortHint) {
        addIndex(BSON("a" << 1));
        runQuerySortHint(fromjson("{$or: [{a: 1}, {a: 2}]}"), BSON("b" << 1), BSON("a" << 1));

        assertNumSolutions(1U);
        assertSolutionExists("{sort: {pattern: {b:1}, limit: 0, node: {fetch: "
                                "{filter: null, node: {ixscan: {filter: null, "
                                "pattern: {a:1}, bounds: {a: [[1,1,true,true], [2,2,true,true]]}}}}}}}");
    }

    //
    // QueryPlannerParams option tests
    //

    TEST_F(QueryPlannerTest, NoBlockingSortsAllowedTest) {
        params.options = QueryPlannerParams::NO_BLOCKING_SORT;
        runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());
        assertNumSolutions(0U);

        addIndex(BSON("x" << 1));

        runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());
        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NoTableScanBasic) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        runQuery(BSONObj());
        assertNumSolutions(0U);

        addIndex(BSON("x" << 1));

        runQuery(BSONObj());
        assertNumSolutions(0U);

        runQuery(fromjson("{x: {$gte: 0}}"));
        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
    }

    TEST_F(QueryPlannerTest, NoTableScanOrWithAndChild) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN;
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]}"));

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{fetch: {filter: null, node: {or: {nodes: ["
                                "{ixscan: {filter: null, pattern: {a: 1}}}, "
                                "{fetch: {filter: {b: 7}, node: {ixscan: "
                                "{filter: null, pattern: {a: 1}}}}}]}}}}");
    }

    //
    // Index Intersection.
    //
    // We don't exhaustively check all plans here.  Instead we check that there exists an
    // intersection plan.  The blending of >1 index plans and ==1 index plans is under development
    // but we want to make sure that we create an >1 index plan when we should.
    //

    TEST_F(QueryPlannerTest, IntersectBasicTwoPred) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        runQuery(fromjson("{a:1, b:{$gt: 1}}"));

        assertSolutionExists("{fetch: {filter: null, node: {andHash: {nodes: ["
                                    "{ixscan: {filter: null, pattern: {a:1}}},"
                                    "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
    }

    TEST_F(QueryPlannerTest, IntersectBasicTwoPredCompound) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
        addIndex(BSON("a" << 1 << "c" << 1));
        addIndex(BSON("b" << 1));
        runQuery(fromjson("{a:1, b:1, c:1}"));

        // There's an andSorted not andHash because the two seeks are point intervals.
        assertSolutionExists("{fetch: {filter: null, node: {andSorted: {nodes: ["
                                    "{ixscan: {filter: null, pattern: {a:1, c:1}}},"
                                    "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
    }

    // SERVER-12196
    TEST_F(QueryPlannerTest, IntersectBasicTwoPredCompoundMatchesIdxOrder1) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
        addIndex(BSON("a" << 1 << "b" << 1));
        addIndex(BSON("b" << 1));
        runQuery(fromjson("{a:1, b:1}"));

        assertNumSolutions(3U);

        assertSolutionExists("{fetch: {filter: null, node: "
                                 "{ixscan: {filter: null, pattern: {a:1, b:1}}}}}");
        assertSolutionExists("{fetch: {filter: {a:1}, node: "
                                 "{ixscan: {filter: null, pattern: {b:1}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {andHash: {nodes: ["
                                 "{ixscan: {filter: null, pattern: {a:1, b:1}}},"
                                 "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
    }

    // SERVER-12196
    TEST_F(QueryPlannerTest, IntersectBasicTwoPredCompoundMatchesIdxOrder2) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
        addIndex(BSON("b" << 1));
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{a:1, b:1}"));

        assertNumSolutions(3U);

        assertSolutionExists("{fetch: {filter: null, node: "
                                 "{ixscan: {filter: null, pattern: {a:1, b:1}}}}}");
        assertSolutionExists("{fetch: {filter: {a:1}, node: "
                                 "{ixscan: {filter: null, pattern: {b:1}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {andHash: {nodes: ["
                                 "{ixscan: {filter: null, pattern: {a:1, b:1}}},"
                                 "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
    }

    TEST_F(QueryPlannerTest, IntersectBasicMultikey) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
        // True means multikey.
        addIndex(BSON("a" << 1), true);
        // We can't combine bounds for the multikey case so we have one scan per pred.
        runQuery(fromjson("{a:1, a:2}"));
        assertSolutionExists("{fetch: {filter: null, node: {andSorted: {nodes: ["
                                    "{ixscan: {filter: null, pattern: {a:1}}},"
                                    "{ixscan: {filter: null, pattern: {a:1}}}]}}}}");
    }

    TEST_F(QueryPlannerTest, IntersectSubtreeNodes) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        addIndex(BSON("c" << 1));
        addIndex(BSON("d" << 1));

        runQuery(fromjson("{$or: [{a: 1}, {b: 1}], $or: [{c:1}, {d:1}]}"));
        assertSolutionExists("{fetch: {filter: null, node: {andHash: {nodes: ["
                                    "{or: {nodes: [{ixscan:{filter:null, pattern:{a:1}}},"
                                          "{ixscan:{filter:null, pattern:{b:1}}}]}},"
                                    "{or: {nodes: [{ixscan:{filter:null, pattern:{c:1}}},"
                                          "{ixscan:{filter:null, pattern:{d:1}}}]}}]}}}}");
    }

    TEST_F(QueryPlannerTest, IntersectSubtreeAndPred) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        addIndex(BSON("c" << 1));
        runQuery(fromjson("{a: 1, $or: [{b:1}, {c:1}]}"));
        assertSolutionExists("{fetch: {filter: null, node: {andHash: {nodes:["
                                    "{or: {nodes: [{ixscan:{filter:null, pattern:{b:1}}},"
                                          "{ixscan:{filter:null, pattern:{c:1}}}]}},"
                                    "{ixscan:{filter: null, pattern:{a:1}}}]}}}}");
    }

    TEST_F(QueryPlannerTest, IntersectElemMatch) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
        addIndex(BSON("a.b" << 1));
        addIndex(BSON("a.c" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));
        assertSolutionExists("{fetch: {filter: {a:{$elemMatch:{b:1, c:1}}},"
                                 "node: {andSorted: {nodes: ["
                                         "{ixscan: {filter: null, pattern: {'a.b':1}}},"
                                         "{ixscan: {filter: null, pattern: {'a.c':1}}}]}}}}");
    }

    TEST_F(QueryPlannerTest, IntersectSortFromAndHash) {
        params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        runQuerySortProj(fromjson("{a: 1, b:{$gt: 1}}"), fromjson("{b:1}"), BSONObj());

        // This provides the sort.
        assertSolutionExists("{fetch: {filter: null, node: {andHash: {nodes: ["
                                    "{ixscan: {filter: null, pattern: {a:1}}},"
                                    "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");

        // Rearrange the preds, shouldn't matter.
        runQuerySortProj(fromjson("{b: 1, a:{$lt: 7}}"), fromjson("{b:1}"), BSONObj());
        assertSolutionExists("{fetch: {filter: null, node: {andHash: {nodes: ["
                                    "{ixscan: {filter: null, pattern: {a:1}}},"
                                    "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
    }

    //
    // Test that we add a KeepMutations when we should and and we don't add one when we shouldn't.
    //

    // Collection scan doesn't keep any state, so it can't produce flagged data.
    TEST_F(QueryPlannerTest, NoMutationsForCollscan) {
        params.options = QueryPlannerParams::KEEP_MUTATIONS;
        runQuery(fromjson(""));
        assertSolutionExists("{cscan: {dir: 1}}");
    }

    // Collscan + sort doesn't produce flagged data either.
    TEST_F(QueryPlannerTest, NoMutationsForSort) {
        params.options = QueryPlannerParams::KEEP_MUTATIONS;
        runQuerySortProj(fromjson(""), fromjson("{a:1}"), BSONObj());
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
    }

    // An index scan + fetch requires a keep node as it can flag data.  Also make sure we put it in
    // the right place, under the sort.
    TEST_F(QueryPlannerTest, MutationsFromFetch) {
        params.options = QueryPlannerParams::KEEP_MUTATIONS;
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: 5}"), fromjson("{b:1}"), BSONObj());
        assertSolutionExists("{sort: {pattern: {b:1}, limit: 0, node: {keep: {node: "
                                 "{fetch: {node: {ixscan: {pattern: {a:1}}}}}}}}}");
    }

    // Index scan w/covering doesn't require a keep node as there's no fetch.
    TEST_F(QueryPlannerTest, NoFetchNoKeep) {
        params.options = QueryPlannerParams::KEEP_MUTATIONS;
        addIndex(BSON("x" << 1));
        // query, sort, proj
        runQuerySortProj(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{_id: 0, x: 1}"));

        // cscan is a soln but we override the params that say to include it.
        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{proj: {spec: {_id: 0, x: 1}, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
    }

    // No keep with geoNear.
    TEST_F(QueryPlannerTest, NoKeepWithGeoNear) {
        params.options = QueryPlannerParams::KEEP_MUTATIONS;
        addIndex(BSON("a" << "2d"));
        runQuery(fromjson("{a: {$near: [0,0], $maxDistance:0.3 }}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{geoNear2d: {a: '2d'}}");
    }

    // No keep when we have an indexed sort.
    TEST_F(QueryPlannerTest, NoKeepWithIndexedSort) {
        params.options = QueryPlannerParams::KEEP_MUTATIONS;
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProjSkipLimit(fromjson("{a: {$in: [1, 2]}}"),
                                  BSON("b" << 1), BSONObj(), 0, 1);

        // cscan solution exists but we didn't turn on the "always include a collscan."
        assertNumSolutions(1);
        assertSolutionExists("{fetch: {node: {mergeSort: {nodes: "
                                "[{ixscan: {pattern: {a: 1, b: 1}}}, {ixscan: {pattern: {a: 1, b: 1}}}]}}}}");
    }

    //
    // Test bad input to query planner helpers.
    //

    TEST(BadInputTest, CacheDataFromTaggedTree) {
        PlanCacheIndexTree* indexTree;

        // Null match expression.
        vector<IndexEntry> relevantIndices;
        Status s = QueryPlanner::cacheDataFromTaggedTree(NULL, relevantIndices, &indexTree);
        ASSERT_NOT_OK(s);
        ASSERT(NULL == indexTree);

        // No relevant index matching the index tag.
        relevantIndices.push_back(IndexEntry(BSON("a" << 1), false, false, "a_1"));

        CanonicalQuery *cq;
        Status cqStatus = CanonicalQuery::canonicalize(ns, BSON("a" << 3), &cq);
        ASSERT_OK(cqStatus);
        boost::scoped_ptr<CanonicalQuery> scopedCq(cq);
        scopedCq->root()->setTag(new IndexTag(1));

        s = QueryPlanner::cacheDataFromTaggedTree(scopedCq->root(), relevantIndices, &indexTree);
        ASSERT_NOT_OK(s);
        ASSERT(NULL == indexTree);
    }

    TEST(BadInputTest, TagAccordingToCache) {
        CanonicalQuery *cq;
        Status cqStatus = CanonicalQuery::canonicalize(ns, BSON("a" << 3), &cq);
        ASSERT_OK(cqStatus);
        boost::scoped_ptr<CanonicalQuery> scopedCq(cq);

        PlanCacheIndexTree* indexTree = new PlanCacheIndexTree();
        indexTree->setIndexEntry(IndexEntry(BSON("a" << 1), false, false, "a_1"));

        map<BSONObj, size_t> indexMap;

        // Null filter.
        Status s = QueryPlanner::tagAccordingToCache(NULL, indexTree, indexMap);
        ASSERT_NOT_OK(s);

        // Null indexTree.
        s = QueryPlanner::tagAccordingToCache(scopedCq->root(), NULL, indexMap);
        ASSERT_NOT_OK(s);

        // Index not found.
        s = QueryPlanner::tagAccordingToCache(scopedCq->root(), indexTree, indexMap);
        ASSERT_NOT_OK(s);

        // Index found once added to the map.
        indexMap[BSON("a" << 1)] = 0;
        s = QueryPlanner::tagAccordingToCache(scopedCq->root(), indexTree, indexMap);
        ASSERT_OK(s);

        // Regenerate canonical query in order to clear tags.
        cqStatus = CanonicalQuery::canonicalize(ns, BSON("a" << 3), &cq);
        ASSERT_OK(cqStatus);
        scopedCq.reset(cq);

        // Mismatched tree topology.
        PlanCacheIndexTree* child = new PlanCacheIndexTree();
        child->setIndexEntry(IndexEntry(BSON("a" << 1), false, false, "a_1"));
        indexTree->children.push_back(child);
        s = QueryPlanner::tagAccordingToCache(scopedCq->root(), indexTree, indexMap);
        ASSERT_NOT_OK(s);
    }

}  // namespace

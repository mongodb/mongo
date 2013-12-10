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

#include <ostream>
#include <sstream>
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

    TEST(QueryPlannerOptions, NoBlockingSortsAllowedTest) {
        QueryPlannerParams params;
        params.options = QueryPlannerParams::NO_BLOCKING_SORT;
        vector<QuerySolution*> solns;
        CanonicalQuery* rawCQ;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, BSONObj(), fromjson("{x:1}"), BSONObj(), &rawCQ));

        // No indices available to provide the sort.
        QueryPlanner::plan(*rawCQ, params, &solns);
        ASSERT_EQUALS(solns.size(), 0U);

        // Add an index that provides the desired sort.
        params.indices.push_back(IndexEntry(fromjson("{x:1}"), false, false, "foo"));
        QueryPlanner::plan(*rawCQ, params, &solns);
        ASSERT_EQUALS(solns.size(), 1U);
    }

    class IndexAssignmentTest : public mongo::unittest::Test {
    protected:
        void setUp() {
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

        void addIndex(BSONObj keyPattern) {
            // The first false means not multikey.
            // The second false means not sparse.
            // The third arg is the index name and I am egotistical.
            params.indices.push_back(IndexEntry(keyPattern, false, false, "hari_king_of_the_stove"));
        }

        void addIndex(BSONObj keyPattern, bool multikey, bool sparse) {
            params.indices.push_back(IndexEntry(keyPattern, multikey, sparse, "note_to_self_dont_break_build"));
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

            runQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj);
        }

        void runQuerySortProjSkipLimitHint(const BSONObj& query,
                                           const BSONObj& sort, const BSONObj& proj,
                                           long long skip, long long limit,
                                           const BSONObj& hint) {
            runQueryFull(query, sort, proj, skip, limit, hint, BSONObj(), BSONObj());
        }

        void runQueryFull(const BSONObj& query,
                          const BSONObj& sort, const BSONObj& proj,
                          long long skip, long long limit,
                          const BSONObj& hint,
                          const BSONObj& minObj,
                          const BSONObj& maxObj) {
            solns.clear();
            Status s = CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, hint,
                                                    minObj, maxObj, &cq);
            if (!s.isOK()) { cq = NULL; }
            ASSERT_OK(s);
            params.options = QueryPlannerParams::INCLUDE_COLLSCAN;
            s = QueryPlanner::plan(*cq, params, &solns);
            ASSERT_OK(s);
        }

        /**
         * Same as runQuery* functions except we expect a failed status from the planning stage.
         */
        void runInvalidQuery(BSONObj query) {
            runInvalidQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), 0, 0);
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
            runInvalidQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj);
        }

        void runInvalidQuerySortProjSkipLimitHint(const BSONObj& query,
                                                  const BSONObj& sort, const BSONObj& proj,
                                                  long long skip, long long limit,
                                                  const BSONObj& hint) {
            runInvalidQueryFull(query, sort, proj, skip, limit, hint, BSONObj(), BSONObj());
        }

        void runInvalidQueryFull(const BSONObj& query,
                                 const BSONObj& sort, const BSONObj& proj,
                                 long long skip, long long limit,
                                 const BSONObj& hint,
                                 const BSONObj& minObj,
                                 const BSONObj& maxObj) {
            solns.clear();
            Status s = CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, hint,
                                                    minObj, maxObj, &cq);
            if (!s.isOK()) { cq = NULL; }
            ASSERT_OK(s);
            params.options = QueryPlannerParams::INCLUDE_COLLSCAN;
            s = QueryPlanner::plan(*cq, params, &solns);
            ASSERT_NOT_OK(s);
        }

        //
        // Introspect solutions.
        //

        size_t getNumSolutions() const {
            return solns.size();
        }

        void dumpSolutions(ostream& ost) const {
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                    it != solns.end();
                    ++it) {
                ost << (*it)->toString() << endl;
            }
        }

        void dumpSolutions() const {
            dumpSolutions(std::cout);
        }

        /**
         * Checks number solutions. Generates assertion message
         * containing solution dump if applicable.
         */
        void assertNumSolutions(size_t expectSolutions) const {
            if (getNumSolutions() == expectSolutions) {
                return;
            }
            std::stringstream ss;
            ss << "expected " << expectSolutions << " solutions but got " << getNumSolutions()
               << " instead. solutions generated: " << std::endl;
            dumpSolutions(ss);
            FAIL(ss.str());
        }

        /**
         * Looks in the children stored in the 'nodes' field of 'testSoln'
         * to see if they match the 'children' field of 'trueSoln'.
         *
         * This does an unordered comparison, i.e. childrenMatch returns true
         * as long as the set of subtrees in testSoln's 'nodes' matches the set of
         * subtrees in trueSoln's 'children' vector.
         */
        bool childrenMatch(const BSONObj& testSoln, const QuerySolutionNode* trueSoln) const {
            BSONElement children = testSoln["nodes"];
            if (children.eoo() || !children.isABSONObj()) { return false; }

            // The order of the children array in testSoln might not match
            // the order in trueSoln, so we have to check all combos with
            // these nested loops.
            BSONObjIterator i(children.Obj());
            while (i.more()) {
                BSONElement child = i.next();
                if (child.eoo() || !child.isABSONObj()) { return false; }

                // try to match against one of the QuerySolutionNode's children
                bool found = false;
                for (size_t j = 0; j < trueSoln->children.size(); ++j) {
                    if (solutionMatches(child.Obj(), trueSoln->children[j])) {
                        found = true;
                        break;
                    }
                }

                // we couldn't match child
                if (!found) { return false; }
            }

            return true;
        }

        bool filterMatches(const BSONObj& testFilter,
                           const QuerySolutionNode* trueFilterNode) const {
            if (NULL == trueFilterNode->filter) { return false; }
            StatusWithMatchExpression swme = MatchExpressionParser::parse(testFilter);
            if (!swme.isOK()) {
                return false;
            }
            MatchExpression* root = swme.getValue();
            return trueFilterNode->filter->equivalent(root);
        }

        bool solutionMatches(const BSONObj& testSoln, const QuerySolutionNode* trueSoln) const {

            //
            // leaf nodes
            //
            if (STAGE_COLLSCAN == trueSoln->getType()) {
                const CollectionScanNode* csn = static_cast<const CollectionScanNode*>(trueSoln);
                BSONElement el = testSoln["cscan"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj csObj = el.Obj();

                BSONElement dir = csObj["dir"];
                if (dir.eoo() || !dir.isNumber()) { return false; }
                if (dir.numberInt() != csn->direction) { return false; }

                BSONElement filter = csObj["filter"];
                if (filter.eoo()) {
                    return true;
                }
                else if (filter.isNull()) {
                    return NULL == csn->filter;
                }
                else if (!filter.isABSONObj()) {
                    return false;
                }
                return filterMatches(filter.Obj(), trueSoln);
            }
            else if (STAGE_IXSCAN == trueSoln->getType()) {
                const IndexScanNode* ixn = static_cast<const IndexScanNode*>(trueSoln);
                BSONElement el = testSoln["ixscan"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj ixscanObj = el.Obj();

                BSONElement pattern = ixscanObj["pattern"];
                if (pattern.eoo() || !pattern.isABSONObj()) { return false; }
                if (pattern.Obj() != ixn->indexKeyPattern) { return false; }

                BSONElement filter = ixscanObj["filter"];
                if (filter.eoo()) {
                    return true;
                }
                else if (filter.isNull()) {
                    return NULL == ixn->filter;
                }
                else if (!filter.isABSONObj()) {
                    return false;
                }
                return filterMatches(filter.Obj(), trueSoln);
            }
            else if (STAGE_GEO_2D == trueSoln->getType()) {
                const Geo2DNode* node = static_cast<const Geo2DNode*>(trueSoln);
                BSONElement el = testSoln["geo2d"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj geoObj = el.Obj();
                return geoObj == node->indexKeyPattern;
            }
            else if (STAGE_GEO_NEAR_2D == trueSoln->getType()) {
                const GeoNear2DNode* node = static_cast<const GeoNear2DNode*>(trueSoln);
                BSONElement el = testSoln["geoNear2d"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj geoObj = el.Obj();
                return geoObj == node->indexKeyPattern;
            }
            else if (STAGE_GEO_NEAR_2DSPHERE == trueSoln->getType()) {
                const GeoNear2DSphereNode* node = static_cast<const GeoNear2DSphereNode*>(trueSoln);
                BSONElement el = testSoln["geoNear2dsphere"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj geoObj = el.Obj();
                return geoObj == node->indexKeyPattern;
            }

            //
            // internal nodes
            //

            else if (STAGE_FETCH == trueSoln->getType()) {
                const FetchNode* fn = static_cast<const FetchNode*>(trueSoln);

                BSONElement el = testSoln["fetch"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj fetchObj = el.Obj();

                BSONElement filter = fetchObj["filter"];
                if (!filter.eoo()) {
                    if (filter.isNull()) {
                        if (NULL != fn->filter) { return false; }
                    }
                    else if (!filter.isABSONObj()) {
                        return false;
                    }
                    else if (!filterMatches(filter.Obj(), trueSoln)) {
                        return false;
                    }
                }

                BSONElement child = fetchObj["node"];
                if (child.eoo() || !child.isABSONObj()) { return false; }
                return solutionMatches(child.Obj(), fn->children[0]);
            }
            else if (STAGE_OR == trueSoln->getType()) {
                const OrNode * orn = static_cast<const OrNode*>(trueSoln);
                BSONElement el = testSoln["or"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj orObj = el.Obj();
                return childrenMatch(orObj, orn);
            }
            else if (STAGE_PROJECTION == trueSoln->getType()) {
                const ProjectionNode* pn = static_cast<const ProjectionNode*>(trueSoln);

                BSONElement el = testSoln["proj"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj projObj = el.Obj();

                BSONElement spec = projObj["spec"];
                if (spec.eoo() || !spec.isABSONObj()) { return false; }
                BSONElement child = projObj["node"];
                if (child.eoo() || !child.isABSONObj()) { return false; }

                return (spec.Obj() == pn->projection)
                       && solutionMatches(child.Obj(), pn->children[0]);
            }
            else if (STAGE_SORT == trueSoln->getType()) {
                const SortNode* sn = static_cast<const SortNode*>(trueSoln);
                BSONElement el = testSoln["sort"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj sortObj = el.Obj();

                BSONElement patternEl = sortObj["pattern"];
                if (patternEl.eoo() || !patternEl.isABSONObj()) { return false; }
                BSONElement limitEl = sortObj["limit"];
                if (!limitEl.isNumber()) { return false; }
                BSONElement child = sortObj["node"];
                if (child.eoo() || !child.isABSONObj()) { return false; }

                return (patternEl.Obj() == sn->pattern)
                       && (limitEl.numberInt() == sn->limit)
                       && solutionMatches(child.Obj(), sn->children[0]);
            }
            else if (STAGE_SORT_MERGE == trueSoln->getType()) {
                const MergeSortNode* msn = static_cast<const MergeSortNode*>(trueSoln);
                BSONElement el = testSoln["mergeSort"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj mergeSortObj = el.Obj();
                return childrenMatch(mergeSortObj, msn);
            }
            else if (STAGE_SKIP == trueSoln->getType()) {
                const SkipNode* sn = static_cast<const SkipNode*>(trueSoln);
                BSONElement el = testSoln["skip"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj sortObj = el.Obj();

                BSONElement skipEl = sortObj["n"];
                if (!skipEl.isNumber()) { return false; }
                BSONElement child = sortObj["node"];
                if (child.eoo() || !child.isABSONObj()) { return false; }

                return (skipEl.numberInt() == sn->skip)
                       && solutionMatches(child.Obj(), sn->children[0]);
            }
            else if (STAGE_LIMIT == trueSoln->getType()) {
                const LimitNode* ln = static_cast<const LimitNode*>(trueSoln);
                BSONElement el = testSoln["limit"];
                if (el.eoo() || !el.isABSONObj()) { return false; }
                BSONObj sortObj = el.Obj();

                BSONElement limitEl = sortObj["n"];
                if (!limitEl.isNumber()) { return false; }
                BSONElement child = sortObj["node"];
                if (child.eoo() || !child.isABSONObj()) { return false; }

                return (limitEl.numberInt() == ln->limit)
                       && solutionMatches(child.Obj(), ln->children[0]);
            }

            return false;
        }

        /**
         * Verifies that the solution tree represented in json by 'solnJson' is
         * one of the solutions generated by QueryPlanner.
         */
        void assertSolutionExists(const string& solnJson) const {
            BSONObj testSoln = fromjson(solnJson);
            size_t matches = 0;
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                    it != solns.end();
                    ++it) {
                QuerySolutionNode* root = (*it)->root.get();
                if (solutionMatches(testSoln, root)) {
                    ++matches;
                }
            }
            if (matches == 1) {
                return;
            }
            std::stringstream ss;
            ss << "expected a single match for solution " << solnJson
               << " but got " << matches
               << " instead. all solutions generated: " << std::endl;
            dumpSolutions(ss);
            FAIL(ss.str());
        }

        // TODO:
        // bool hasIndexedPlan(BSONObj indexKeyPattern);

        void getAllPlans(StageType stageType, vector<QuerySolution*>* out) const {
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                 it != solns.end();
                 ++it) {
                if ((*it)->root->getType() == stageType) {
                    out->push_back(*it);
                }
            }
        }

        BSONObj queryObj;
        CanonicalQuery* cq;
        QueryPlannerParams params;
        vector<QuerySolution*> solns;
    };

    //
    // Equality
    //

    TEST_F(IndexAssignmentTest, EqualityIndexScan) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << 5));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, EqualityIndexScanWithTrailingFields) {
        addIndex(BSON("x" << 1 << "y" << 1));

        runQuery(BSON("x" << 5));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1, y: 1}}}}}");
    }

    //
    // <
    //

    TEST_F(IndexAssignmentTest, LessThan) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << BSON("$lt" << 5)));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: {$lt: 5}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");
    }

    //
    // <=
    //

    TEST_F(IndexAssignmentTest, LessThanEqual) {
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

    TEST_F(IndexAssignmentTest, GreaterThan) {
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

    TEST_F(IndexAssignmentTest, GreaterThanEqual) {
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

    TEST_F(IndexAssignmentTest, Mod) {
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

    TEST_F(IndexAssignmentTest, ExistsTrue) {
        addIndex(BSON("x" << 1));

        runQuery(fromjson("{x: 1, y: {$exists: true}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ExistsFalse) {
        addIndex(BSON("x" << 1));

        runQuery(fromjson("{x: 1, y: {$exists: false}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ExistsTrueSparseIndex) {
        addIndex(BSON("x" << 1), false, true);

        runQuery(fromjson("{x: 1, y: {$exists: true}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ExistsFalseSparseIndex) {
        addIndex(BSON("x" << 1), false, true);

        runQuery(fromjson("{x: 1, y: {$exists: false}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
    }

    //
    // skip and limit
    //

    TEST_F(IndexAssignmentTest, BasicSkipNoIndex) {
        addIndex(BSON("a" << 1));

        runQuerySkipLimit(BSON("x" << 5), 3, 0);

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{skip: {n: 3, node: {cscan: {dir: 1, filter: {x: 5}}}}}");
    }

    TEST_F(IndexAssignmentTest, BasicSkipWithIndex) {
        addIndex(BSON("a" << 1 << "b" << 1));

        runQuerySkipLimit(BSON("a" << 5), 8, 0);

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{skip: {n: 8, node: {cscan: {dir: 1, filter: {a: 5}}}}}");
        assertSolutionExists("{skip: {n: 8, node: {fetch: {filter: null, node: "
                                "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}");
    }

    TEST_F(IndexAssignmentTest, BasicLimitNoIndex) {
        addIndex(BSON("a" << 1));

        runQuerySkipLimit(BSON("x" << 5), 0, -3);

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{limit: {n: 3, node: {cscan: {dir: 1, filter: {x: 5}}}}}");
    }

    TEST_F(IndexAssignmentTest, BasicSoftLimitNoIndex) {
        addIndex(BSON("a" << 1));

        runQuerySkipLimit(BSON("x" << 5), 0, 3);

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
    }

    TEST_F(IndexAssignmentTest, BasicLimitWithIndex) {
        addIndex(BSON("a" << 1 << "b" << 1));

        runQuerySkipLimit(BSON("a" << 5), 0, -5);

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{limit: {n: 5, node: {cscan: {dir: 1, filter: {a: 5}}}}}");
        assertSolutionExists("{limit: {n: 5, node: {fetch: {filter: null, node: "
                                "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}");
    }

    TEST_F(IndexAssignmentTest, BasicSoftLimitWithIndex) {
        addIndex(BSON("a" << 1 << "b" << 1));

        runQuerySkipLimit(BSON("a" << 5), 0, 5);

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: 5}}}}");
        assertSolutionExists("{fetch: {filter: null, node: "
                                "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, SkipAndLimit) {
        addIndex(BSON("x" << 1));

        runQuerySkipLimit(BSON("x" << BSON("$lte" << 4)), 7, -2);

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{limit: {n: 2, node: {skip: {n: 7, node: "
                                "{cscan: {dir: 1, filter: {x: {$lte: 4}}}}}}}}");
        assertSolutionExists("{limit: {n: 2, node: {skip: {n: 7, node: {fetch: "
                                "{filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}}}}}");
    }

    TEST_F(IndexAssignmentTest, SkipAndSoftLimit) {
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

    TEST_F(IndexAssignmentTest, TwoPredicatesAnding) {
        addIndex(BSON("x" << 1));

        runQuery(fromjson("{$and: [ {x: {$gt: 1}}, {x: {$lt: 3}} ] }"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, SimpleOr) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{a: 20}, {a: 21}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a: 20}, {a: 21}]}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a:1}}}}}");
    }

    TEST_F(IndexAssignmentTest, OrWithoutEnoughIndices) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{a: 20}, {b: 21}]}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a: 20}, {b: 21}]}}}");
    }

    TEST_F(IndexAssignmentTest, OrWithAndChild) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {or: {nodes: ["
                                "{ixscan: {filter: null, pattern: {a: 1}}}, "
                                "{fetch: {filter: {b: 7}, node: {ixscan: "
                                "{filter: null, pattern: {a: 1}}}}}]}}}}");
    }

    TEST_F(IndexAssignmentTest, AndWithUnindexedOrChild) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a:20, $or: [{b:1}, {c:7}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, node: "
                                "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    }


    TEST_F(IndexAssignmentTest, AndWithOrWithOneIndex) {
        addIndex(BSON("b" << 1));
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{b:1}, {c:7}], a:20}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    //
    // Hint
    //

    TEST_F(IndexAssignmentTest, HintValid) {
        addIndex(BSON("a" << 1));
        runQueryHint(BSONObj(), fromjson("{a: 1}"));

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, HintInvalid) {
        addIndex(BSON("a" << 1));
        runInvalidQueryHint(BSONObj(), fromjson("{b: 1}"));
    }

    //
    // Min/Max
    //

    TEST_F(IndexAssignmentTest, MinValid) {
        addIndex(BSON("a" << 1));
        runQueryHintMinMax(BSONObj(), BSONObj(), fromjson("{a: 1}"), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, MinWithoutIndex) {
        runInvalidQueryHintMinMax(BSONObj(), BSONObj(), fromjson("{a: 1}"), BSONObj());
    }

    TEST_F(IndexAssignmentTest, MinBadHint) {
        addIndex(BSON("b" << 1));
        runInvalidQueryHintMinMax(BSONObj(), fromjson("{b: 1}"), fromjson("{a: 1}"), BSONObj());
    }

    TEST_F(IndexAssignmentTest, MaxValid) {
        addIndex(BSON("a" << 1));
        runQueryHintMinMax(BSONObj(), BSONObj(), BSONObj(), fromjson("{a: 1}"));

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, "
                                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, MaxWithoutIndex) {
        runInvalidQueryHintMinMax(BSONObj(), BSONObj(), BSONObj(), fromjson("{a: 1}"));
    }

    TEST_F(IndexAssignmentTest, MaxBadHint) {
        addIndex(BSON("b" << 1));
        runInvalidQueryHintMinMax(BSONObj(), fromjson("{b: 1}"), BSONObj(), fromjson("{a: 1}"));
    }

    //
    // Tree operations that require simple tree rewriting.
    //

    TEST_F(IndexAssignmentTest, AndOfAnd) {
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

    TEST_F(IndexAssignmentTest, EquivalentAndsOne) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{$and: [{a: 1}, {b: {$all: [10, 20]}}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and:[{a:1},{b:10},{b:20}]}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, EquivalentAndsTwo) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{$and: [{a: 1, b: 10}, {a: 1, b: 20}]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and:[{a:1},{b:10},{a:1},{b:20}]}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    //
    // Covering
    //

    TEST_F(IndexAssignmentTest, BasicCovering) {
        addIndex(BSON("x" << 1));
        // query, sort, proj
        runQuerySortProj(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{_id: 0, x: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, x: 1}, node: {ixscan: "
                                "{filter: null, pattern: {x: 1}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, x: 1}, node: "
                                "{cscan: {dir: 1, filter: {x:{$gt:1}}}}}}");
    }

    TEST_F(IndexAssignmentTest, DottedFieldCovering) {
        addIndex(BSON("a.b" << 1));
        runQuerySortProj(fromjson("{'a.b': 5}"), BSONObj(), fromjson("{_id: 0, 'a.b': 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, 'a.b': 1}, node: "
                                "{cscan: {dir: 1, filter: {'a.b': 5}}}}}");
        // SERVER-2104
        //assertSolutionExists("{proj: {spec: {_id: 0, 'a.b': 1}, node: {'a.b': 1}}}");
    }

    TEST_F(IndexAssignmentTest, IdCovering) {
        runQuerySortProj(fromjson("{_id: {$gt: 10}}"), BSONObj(), fromjson("{_id: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 1}, node: "
                                "{cscan: {dir: 1, filter: {_id: {$gt: 10}}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 1}, node: {ixscan: "
                                "{filter: null, pattern: {_id: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ProjNonCovering) {
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

    TEST_F(IndexAssignmentTest, BasicSort) {
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

    TEST_F(IndexAssignmentTest, BasicSortBooleanIndexKeyPattern) {
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

    TEST_F(IndexAssignmentTest, SortLimit) {
        // Negative limit indicates hard limit - see lite_parsed_query.cpp
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 0, -3);
        assertNumSolutions(1U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 3, "
                                "node: {cscan: {dir: 1}}}}");
    }

    TEST_F(IndexAssignmentTest, SortSkip) {
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, 0);
        assertNumSolutions(1U);
        // If only skip is provided, do not limit sort.
        assertSolutionExists("{skip: {n: 2, node: "
                                "{sort: {pattern: {a: 1}, limit: 0, "
                                "node: {cscan: {dir: 1}}}}}}");
    }

    TEST_F(IndexAssignmentTest, SortSkipLimit) {
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, -3);
        assertNumSolutions(1U);
        // Limit in sort node should be adjusted by skip count
        assertSolutionExists("{skip: {n: 2, node: "
                                "{sort: {pattern: {a: 1}, limit: 5, "
                                "node: {cscan: {dir: 1}}}}}}");
    }

    TEST_F(IndexAssignmentTest, SortSoftLimit) {
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 0, 3);
        assertNumSolutions(1U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 3, "
                                "node: {cscan: {dir: 1}}}}");
    }

    TEST_F(IndexAssignmentTest, SortSkipSoftLimit) {
        runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, 3);
        assertNumSolutions(1U);
        assertSolutionExists("{skip: {n: 2, node: "
                                "{sort: {pattern: {a: 1}, limit: 5, "
                                "node: {cscan: {dir: 1}}}}}}");
    }

    //
    // Basic sort elimination
    //

    TEST_F(IndexAssignmentTest, BasicSortElim) {
        addIndex(BSON("x" << 1));
        // query, sort, proj
        runQuerySortProj(fromjson("{ x : {$gt: 1}}"), fromjson("{x: 1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {x: 1}, limit: 0, "
                                "node: {cscan: {dir: 1, filter: {x: {$gt: 1}}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {x: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, SortElimCompound) {
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

    TEST_F(IndexAssignmentTest, BasicCompound) {
        addIndex(BSON("x" << 1 << "y" << 1));
        runQuery(fromjson("{ x : 5, y: 10}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1, y: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, CompoundMissingField) {
        addIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
        runQuery(fromjson("{ x : 5, z: 10}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: {z: 10}, node: "
                                "{ixscan: {filter: null, pattern: {x: 1, y: 1, z: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, CompoundFieldsOrder) {
        addIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
        runQuery(fromjson("{ x : 5, z: 10, y:1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {x: 1, y: 1, z: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, CantUseCompound) {
        addIndex(BSON("x" << 1 << "y" << 1));
        runQuery(fromjson("{ y: 10}"));

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{cscan: {dir: 1, filter: {y: 10}}}");
    }

    //
    // Array operators
    //

    TEST_F(IndexAssignmentTest, ElemMatchOneField) {
        addIndex(BSON("a.b" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1}}}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a:{$elemMatch:{b:1}}}}}");
        assertSolutionExists("{fetch: {filter: {a:{$elemMatch:{b:1}}}, node: "
                                "{ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ElemMatchTwoFields) {
        addIndex(BSON("a.b" << 1));
        addIndex(BSON("a.c" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));

        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a:{$elemMatch:{b:1,c:1}}}}}");
        assertSolutionExists("{fetch: {filter: {a:{$elemMatch:{b:1,c:1}}}, node: "
                                "{ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
        assertSolutionExists("{fetch: {filter: {a:{$elemMatch:{c:1,b:1}}}, node: "
                                "{ixscan: {filter: null, pattern: {'a.c': 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, BasicAllElemMatch) {
        addIndex(BSON("foo.a" << 1));
        addIndex(BSON("foo.b" << 1));
        runQuery(fromjson("{foo: {$all: [ {$elemMatch: {a:1, b:1}}, {$elemMatch: {a:2, b:2}}]}}"));

        ASSERT_EQUALS(getNumSolutions(), 5U);
        assertSolutionExists("{cscan: {dir: 1, filter: {foo:{$all:"
                                "[{$elemMatch:{a:1,b:1}},{$elemMatch:{a:2,b:2}}]}}}}");
        assertSolutionExists("{fetch: {filter: {foo:{$all:[{$elemMatch:{a:1,b:1}},{$elemMatch:{a:2,b:2}}]}}, "
                                "node: {ixscan: {filter: null, pattern: {'foo.a': 1}}}}}");
        assertSolutionExists("{fetch: {filter: {foo:{$all:[{$elemMatch:{a:2,b:2}},{$elemMatch:{a:1,b:1}}]}}, "
                                "node: {ixscan: {filter: null, pattern: {'foo.a': 1}}}}}");
        assertSolutionExists("{fetch: {filter: {foo:{$all:[{$elemMatch:{b:1,a:1}},{$elemMatch:{a:2,b:2}}]}}, "
                                "node: {ixscan: {filter: null, pattern: {'foo.b': 1}}}}}");
        assertSolutionExists("{fetch: {filter: {foo:{$all:[{$elemMatch:{b:2,a:2}},{$elemMatch:{a:1,b:1}}]}}, "
                                "node: {ixscan: {filter: null, pattern: {'foo.b': 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ElemMatchValueMatch) {
        addIndex(BSON("foo" << 1));
        addIndex(BSON("foo" << 1 << "bar" << 1));
        runQuery(fromjson("{foo: {$elemMatch: {$gt: 5, $lt: 10}}}"));

        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1, filter: {foo:{$elemMatch:{$gt:5,$lt:10}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {foo: 1}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {foo: 1, bar: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ElemMatchNested) {
        addIndex(BSON("a.b.c" << 1));
        runQuery(fromjson("{ a:{ $elemMatch:{ b:{ $elemMatch:{ c:{ $gte:1, $lte:1 } } } } }}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, TwoElemMatchNested) {
        addIndex(BSON("a.d.e" << 1));
        addIndex(BSON("a.b.c" << 1));
        runQuery(fromjson("{ a:{ $elemMatch:{ d:{ $elemMatch:{ e:{ $lte:1 } } },"
                             "b:{ $elemMatch:{ c:{ $gte:1 } } } } } }"));

        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.d.e': 1}}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ElemMatchCompoundTwoFields) {
        addIndex(BSON("a.b" << 1 << "a.c" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ArrayEquality) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a : [1, 2, 3]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a:[1,2,3]}}}");
        assertSolutionExists("{fetch: {filter: {a:[1,2,3]}, node: "
                                "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    //
    // Geo
    // http://docs.mongodb.org/manual/reference/operator/query-geospatial/#geospatial-query-compatibility-chart
    //

    TEST_F(IndexAssignmentTest, Basic2DNonNear) {
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

    TEST_F(IndexAssignmentTest, Basic2DSphereNonNear) {
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

    TEST_F(IndexAssignmentTest, Basic2DGeoNear) {
        // Can only do near + old point.
        addIndex(BSON("a" << "2d"));
        runQuery(fromjson("{a: {$near: [0,0], $maxDistance:0.3 }}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{geoNear2d: {a: '2d'}}");
    }

    TEST_F(IndexAssignmentTest, Basic2DSphereGeoNear) {
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

    TEST_F(IndexAssignmentTest, Basic2DSphereGeoNearReverseCompound) {
        addIndex(BSON("x" << 1));
        addIndex(BSON("x" << 1 << "a" << "2dsphere"));
        runQuery(fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));

        ASSERT_EQUALS(getNumSolutions(), 1U);
        assertSolutionExists("{geoNear2dsphere: {x: 1, a: '2dsphere'}}");
    }

    TEST_F(IndexAssignmentTest, NearNoIndex) {
        addIndex(BSON("x" << 1));
        runInvalidQuery(fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));
    }

    TEST_F(IndexAssignmentTest, TwoDSphereNoGeoPred) {
        addIndex(BSON("x" << 1 << "a" << "2dsphere"));
        runQuery(fromjson("{x:1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1, a: '2dsphere'}}}}}");
    }

    // SERVER-3984, $or 2d index
    TEST_F(IndexAssignmentTest, Or2DNonNear) {
        addIndex(BSON("a" << "2d"));
        addIndex(BSON("b" << "2d"));
        runQuery(fromjson("{$or: [ {a : { $within : { $polygon : [[0,0], [2,0], [4,0]] } }},"
                                 " {b : { $within : { $center : [[ 5, 5 ], 7 ] } }} ]}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {or: {nodes: [{geo2d: {a: '2d'}}, {geo2d: {b: '2d'}}]}}}}");
    }

    // SERVER-3984, $or 2dsphere index
    TEST_F(IndexAssignmentTest, Or2DSphereNonNear) {
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

    TEST_F(IndexAssignmentTest, InBasic) {
        addIndex(fromjson("{a: 1}"));
        runQuery(fromjson("{a: {$in: [1, 2]}}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: {$in: [1, 2]}}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1}}}}}");
    }

    // Logically equivalent to the preceding $in query.
    // Indexed solution should be the same.
    TEST_F(IndexAssignmentTest, InBasicOrEquivalent) {
        addIndex(fromjson("{a: 1}"));
        runQuery(fromjson("{$or: [{a: 1}, {a: 2}]}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a: 1}, {a: 2}]}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, InCompoundIndexFirst) {
        addIndex(fromjson("{a: 1, b: 1}"));
        runQuery(fromjson("{a: {$in: [1, 2]}, b: 3}"));

        assertNumSolutions(2U);
        // TODO: update filter in cscan solution when SERVER-12024 is implemented
        assertSolutionExists("{cscan: {dir: 1, filter: {a: {$in: [1, 2]}, b: 3}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
    }

    // Logically equivalent to the preceding $in query.
    // Indexed solution should be the same.
    // Currently fails - pre-requisite to SERVER-12024
    /*
    TEST_F(IndexAssignmentTest, InCompoundIndexFirstOrEquivalent) {
        addIndex(fromjson("{a: 1, b: 1}"));
        runQuery(fromjson("{$and: [{$or: [{a: 1}, {a: 2}]}, {b: 3}]}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{$or: [{a: 1}, {a: 2}]}, {b: 3}]}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
    }
    */

    TEST_F(IndexAssignmentTest, InCompoundIndexLast) {
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
    TEST_F(IndexAssignmentTest, InCompoundIndexLastOrEquivalent) {
        addIndex(fromjson("{a: 1, b: 1}"));
        runQuery(fromjson("{$and: [{a: 3}, {$or: [{b: 1}, {b: 2}]}]}"));

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{a: 3}, {$or: [{b: 1}, {b: 2}]}]}}}");
        assertSolutionExists("{fetch: {filter: null, "
                             "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
    }
    */

    TEST_F(IndexAssignmentTest, InWithSort) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProjSkipLimit(fromjson("{a: {$in: [3, 1, 8]}}"),
                                  BSON("b" << 1), BSONObj(), 0, 1);

        assertSolutionExists("{sort: {pattern: {b: 1}, limit: 1, "
                             "node: {cscan: {dir: 1}}}}");
        // TODO SERVER-1205 there should be a mergeSort solution
    }

    //
    // Multiple solutions
    //

    TEST_F(IndexAssignmentTest, TwoPlans) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("a" << 1 << "b" << 1));

        runQuery(fromjson("{a:1, b:{$gt:2,$lt:2}}"));

        // 2 indexed solns and one non-indexed
        ASSERT_EQUALS(getNumSolutions(), 3U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and:[{a:1},{b:{$gt:2}},{b:{$lt:2}}]}}}");
        assertSolutionExists("{fetch: {filter: {$and:[{b:{$lt:2}},{b:{$gt:2}}]}, node: "
                                "{ixscan: {filter: null, pattern: {a: 1}}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, TwoPlansElemMatch) {
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

    //
    // Sort orders
    //

    // SERVER-1205.
    TEST_F(IndexAssignmentTest, MergeSort) {
        addIndex(BSON("a" << 1 << "c" << 1));
        addIndex(BSON("b" << 1 << "c" << 1));
        runQuerySortProj(fromjson("{$or: [{a:1}, {b:1}]}"), fromjson("{c:1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {c: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {node: {mergeSort: {nodes: "
                                "[{ixscan: {pattern: {a: 1, c: 1}}}, {ixscan: {pattern: {b: 1, c: 1}}}]}}}}");
    }

    // SERVER-1205 as well.
    TEST_F(IndexAssignmentTest, NoMergeSortIfNoSortWanted) {
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
    TEST_F(IndexAssignmentTest, SortOnGeoQuery) {
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
    TEST_F(IndexAssignmentTest, CompoundGeoNoGeoPredicate) {
        addIndex(BSON("creationDate" << 1 << "foo.bar" << "2dsphere"));
        runQuerySortProj(fromjson("{creationDate: { $gt: 7}}"),
                         fromjson("{creationDate: 1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {creationDate: 1}, limit: 0, "
                                "node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {creationDate: 1, 'foo.bar': '2dsphere'}}}}}");
    }

    // Basic "keep sort in mind with an OR"
    TEST_F(IndexAssignmentTest, MergeSortEvenIfSameIndex) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProj(fromjson("{$or: [{a:1}, {a:7}]}"), fromjson("{b:1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {b: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
        // TODO the second solution should be mergeSort rather than just sort
    }

    TEST_F(IndexAssignmentTest, ReverseScanForSort) {
        addIndex(BSON("_id" << 1));
        runQuerySortProj(BSONObj(), fromjson("{_id: -1}"), BSONObj());

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{sort: {pattern: {_id: -1}, limit: 0, node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {_id: 1}}}}}");
    }

    //
    // Sparse indices, SERVER-8067
    // Each index in this block of tests is sparse.
    //

    TEST_F(IndexAssignmentTest, SparseIndexIgnoreForSort) {
        addIndex(fromjson("{a: 1}"), false, true);
        runQuerySortProj(BSONObj(), fromjson("{a: 1}"), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
    }

    TEST_F(IndexAssignmentTest, SparseIndexHintForSort) {
        addIndex(fromjson("{a: 1}"), false, true);
        runQuerySortHint(BSONObj(), fromjson("{a: 1}"), fromjson("{a: 1}"));

        assertNumSolutions(1U);
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, SparseIndexPreferCompoundIndexForSort) {
        addIndex(fromjson("{a: 1}"), false, true);
        addIndex(fromjson("{a: 1, b: 1}"));
        runQuerySortProj(BSONObj(), fromjson("{a: 1}"), BSONObj());

        assertNumSolutions(2U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, SparseIndexForQuery) {
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

    TEST_F(IndexAssignmentTest, PrefixRegex) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a: /^foo/}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: /^foo/}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, PrefixRegexCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: /^foo/}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {a: /^foo/}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, NonPrefixRegex) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a: /foo/}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {a: /foo/}}}");
        assertSolutionExists("{fetch: {filter: null, node: "
                                "{ixscan: {filter: {a: /foo/}, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, NonPrefixRegexCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: /foo/}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {a: /foo/}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {a: /foo/}, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, NonPrefixRegexAnd) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuery(fromjson("{a: /foo/, b: 2}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{a: /foo/}, {b: 2}]}}}");
        assertSolutionExists("{fetch: {filter: null, node: {ixscan: "
                                "{filter: {a: /foo/}, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, NonPrefixRegexAndCovering) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProj(fromjson("{a: /foo/, b: 2}"), BSONObj(),
                         fromjson("{_id: 0, a: 1, b: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
                                "{cscan: {dir: 1, filter: {$and: [{a: /foo/}, {b: 2}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
                                "{ixscan: {filter: {a: /foo/}, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, NonPrefixRegexOrCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$or: [{a: /0/}, {a: /1/}]}"), BSONObj(),
                         fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {$or: [{a: /0/}, {a: /1/}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {$or: [{a: /0/}, {a: /1/}]}, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, NonPrefixRegexInCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: {$in: [/foo/, /bar/]}}"), BSONObj(),
                         fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {a:{$in:[/foo/,/bar/]}}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {a:{$in:[/foo/,/bar/]}}, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, TwoRegexCompoundIndexCovering) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runQuerySortProj(fromjson("{a: /0/, b: /1/}"), BSONObj(),
                         fromjson("{_id: 0, a: 1, b: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
                                "{cscan: {dir: 1, filter: {$and:[{a:/0/},{b:/1/}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
                                "{ixscan: {filter: {$and:[{a:/0/},{b:/1/}]}, pattern: {a: 1, b: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, TwoRegexSameFieldCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$and: [{a: /0/}, {a: /1/}]}"), BSONObj(),
                         fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {$and:[{a:/0/},{a:/1/}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {$and:[{a:/0/},{a:/1/}]}, pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, ThreeRegexSameFieldCovering) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$and: [{a: /0/}, {a: /1/}, {a: /2/}]}"), BSONObj(),
                         fromjson("{_id: 0, a: 1}"));

        ASSERT_EQUALS(getNumSolutions(), 2U);
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{cscan: {dir: 1, filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}}}}}");
        assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: "
                                "{ixscan: {filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}, pattern: {a: 1}}}}}");
    }

    //
    // Negation
    //

    TEST_F(IndexAssignmentTest, NegationIndexForSort) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: {$ne: 1}}"), fromjson("{a: 1}"), BSONObj());

        assertNumSolutions(2U);
        assertSolutionExists("{sort: {pattern: {a: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, NegationTopLevel) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{a: {$ne: 1}}"), BSONObj(), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{cscan: {dir: 1}}");
    }

    TEST_F(IndexAssignmentTest, NegationOr) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$or: [{a: 1}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{cscan: {dir: 1}}");
    }

    TEST_F(IndexAssignmentTest, NegationOrNotIn) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$or: [{a: 1}, {b: {$nin: [1]}}]}"), BSONObj(), BSONObj());

        assertNumSolutions(1U);
        assertSolutionExists("{cscan: {dir: 1}}");
    }

    TEST_F(IndexAssignmentTest, NegationAndIndexOnEquality) {
        addIndex(BSON("a" << 1));
        runQuerySortProj(fromjson("{$and: [{a: 1}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

        assertNumSolutions(2U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, NegationAndIndexOnEqualityAndNegationBranches) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        runQuerySortProj(fromjson("{$and: [{a: 1}, {b: 2}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

        assertNumSolutions(3U);
        assertSolutionExists("{cscan: {dir: 1}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}");
        assertSolutionExists("{fetch: {node: {ixscan: {pattern: {b: 1}}}}}");
    }

    TEST_F(IndexAssignmentTest, NegationAndIndexOnInEquality) {
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
    TEST_F(IndexAssignmentTest, Negation2DGeoNear) {
        addIndex(BSON("a" << "2d"));
        runQuery(fromjson("{$and: [{a: {$near: [0, 0], $maxDistance: 0.3}}, {b: {$ne: 1}}]}"));
        assertNumSolutions(1U);
        assertSolutionExists("{geoNear2d: {a: '2d'}}");
    }

    //
    // 2DSphere geo negation
    // Filter is embedded in a separate fetch node.
    //
    TEST_F(IndexAssignmentTest, Negation2DSphereGeoNear) {
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

    // STOPPED HERE - need to hook up machinery for multiple indexed predicates
    //                second is not working (until the machinery is in place)
    //
    // TEST_F(IndexAssignmentTest, TwoPredicatesOring) {
    //     addIndex(BSON("x" << 1));
    //     runQuery(fromjson("{$or: [ {a: 1}, {a: 2} ] }"));
    //     ASSERT_EQUALS(getNumSolutions(), 2U);

    //     QuerySolution* collScanSolution;
    //     getPlanByType(STAGE_COLLSCAN, &collScanSolution);
    //     // TODO check filter

    //     BSONObj boundsObj =
    //         fromjson("{x:"
    //                  "   ["
    //                  "     [1, 1, true, true],"
    //                  "     [2, 2, true, true],"
    //                  "   ]"
    //                  "}");

    //     QuerySolution* indexedSolution;
    //     getPlanByType(STAGE_IXSCAN, &indexedSolution);
    //     IndexScanNode* ixNode = static_cast<IndexScanNode*>(indexedSolution->root.get());
    //     boundsEqual(boundsObj, ixNode->bounds);
    //     // TODO check filter
    //}

}  // namespace

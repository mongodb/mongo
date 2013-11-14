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
            solns.clear();
            queryObj = query.getOwned();
            ASSERT_OK(CanonicalQuery::canonicalize(ns, queryObj, &cq));
            params.options = QueryPlannerParams::INCLUDE_COLLSCAN;
            QueryPlanner::plan(*cq, params, &solns);
        }

        void runDetailedQuery(const BSONObj& query, const BSONObj& sort, const BSONObj& proj) {
            solns.clear();
            ASSERT_OK(CanonicalQuery::canonicalize(ns, query, sort, proj, &cq));
            params.options = QueryPlannerParams::INCLUDE_COLLSCAN;
            QueryPlanner::plan(*cq, params, &solns);
            ASSERT_GREATER_THAN(solns.size(), 0U);;
        }

        //
        // Introspect solutions.
        //

        size_t getNumSolutions() const {
            return solns.size();
        }

        void dumpSolutions() const {
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                    it != solns.end();
                    ++it) {
                cout << (*it)->toString() << endl;
            }
        }

        // TODO:
        // bool hasIndexedPlan(BSONObj indexKeyPattern);

        void getPlanByType(StageType stageType, QuerySolution** soln) const {
            size_t found = 0;
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                 it != solns.end();
                 ++it) {
                if ((*it)->root->getType() == stageType) {
                    *soln = *it;
                    found++;
                }
            }
            if (1 != found) {
                cout << "Can't find requested stage type " << stageType
                     << ", dump of all solutions:\n";
                for (vector<QuerySolution*>::const_iterator it = solns.begin();
                        it != solns.end();
                        ++it) {
                    cout << (*it)->toString() << endl;
                }
            }
            ASSERT_EQUALS(found, 1U);
        }

        void getAllPlans(StageType stageType, vector<QuerySolution*>* out) const {
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                 it != solns.end();
                 ++it) {
                if ((*it)->root->getType() == stageType) {
                    out->push_back(*it);
                }
            }
        }

        // { 'field': [ [min, max, startInclusive, endInclusive], ... ], 'field': ... }
        void boundsEqual(BSONObj boundsObj, IndexBounds bounds) const {
            ASSERT_EQUALS(static_cast<int>(bounds.size()), boundsObj.nFields());

            size_t i = 0;
            BSONObjIterator iti(boundsObj);
            while (iti.more()) {

                BSONElement field = iti.next();
                ASSERT_EQUALS(field.type(), Array);
                ASSERT_EQUALS(static_cast<int>(bounds.getNumIntervals(i)),
                              field.embeddedObject().nFields());

                size_t j = 0;
                BSONObjIterator itj(field.embeddedObject());
                while (itj.more()) {

                    BSONElement intervalElem = itj.next();
                    ASSERT_EQUALS(intervalElem.type(), Array);
                    BSONObj intervalObj = intervalElem.embeddedObject();
                    ASSERT_EQUALS(intervalObj.nFields(), 4);

                    Interval interval(intervalObj, intervalObj[2].Bool(), intervalObj[3].Bool());
                    ASSERT_EQUALS(interval.compare(bounds.getInterval(i,j)),
                                  Interval::INTERVAL_EQUALS);

                    j++;
                }

                i++;
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

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        FetchNode* fn = static_cast<FetchNode*>(indexedSolution->root.get());
        IndexScanNode* ixNode = static_cast<IndexScanNode*>(fn->children[0]);
        boundsEqual(fromjson("{x: [ [5, 5, true, true] ] }"), ixNode->bounds);
    }

    TEST_F(IndexAssignmentTest, EqualityIndexScanWithTrailingFields) {
        addIndex(BSON("x" << 1 << "y" << 1));

        runQuery(BSON("x" << 5));

        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    //
    // <
    //

    TEST_F(IndexAssignmentTest, LessThan) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << BSON("$lt" << 5)));

        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    //
    // <=
    //

    TEST_F(IndexAssignmentTest, LessThanEqual) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << BSON("$lte" << 5)));

        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    //
    // >
    //

    TEST_F(IndexAssignmentTest, GreaterThan) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << BSON("$gt" << 5)));

        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    //
    // >=
    //

    TEST_F(IndexAssignmentTest, GreaterThanEqual) {
        addIndex(BSON("x" << 1));

        runQuery(BSON("x" << BSON("$gte" << 5)));

        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    //
    // tree operations
    //

    TEST_F(IndexAssignmentTest, TwoPredicatesAnding) {
        addIndex(BSON("x" << 1));

        runQuery(fromjson("{$and: [ {x: {$gt: 1}}, {x: {$lt: 3}} ] }"));

        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        // This is a fetch not an ixscan because our index tagging isn't good so far and we don't
        // know that the index is used for the second predicate.
        getPlanByType(STAGE_FETCH, &indexedSolution);

        //FetchNode* fn = static_cast<FetchNode*>(indexedSolution->root.get());
        //IndexScanNode* ixNode = static_cast<IndexScanNode*>(fn->child.get());
        // TODO: use this when we tag both indices.
        // boundsEqual(fromjson("{x: [ [1, 3, false, false] ] }"), ixNode->bounds);
        // TODO check filter
    }

    TEST_F(IndexAssignmentTest, SimpleOr) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [ {a: 20}, {a: 21}]}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        QuerySolution* indexedSolution = NULL;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        cout << indexedSolution->toString() << endl;
    }

    TEST_F(IndexAssignmentTest, OrWithoutEnoughIndices) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [ {a: 20}, {b: 21}]}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        cout << collScanSolution->toString() << endl;
    }

    TEST_F(IndexAssignmentTest, OrWithAndChild) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [ {a: 20}, {$and: [{a:1}, {b:7}]}]}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        QuerySolution* indexedSolution = NULL;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        cout << indexedSolution->toString() << endl;
    }

    TEST_F(IndexAssignmentTest, AndWithUnindexedOrChild) {
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{a:20, $or: [{b:1}, {c:7}]}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        QuerySolution* indexedSolution = NULL;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        cout << indexedSolution->toString() << endl;
    }


    TEST_F(IndexAssignmentTest, AndWithOrWithOneIndex) {
        addIndex(BSON("b" << 1));
        addIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{b:1}, {c:7}], a:20}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        QuerySolution* indexedSolution = NULL;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        cout << indexedSolution->toString() << endl;
    }

    //
    // Tree operations that require simple tree rewriting.
    //

    TEST_F(IndexAssignmentTest, AndOfAnd) {
        addIndex(BSON("x" << 1));
        runQuery(fromjson("{$and: [ {$and: [ {x: 2.5}]}, {x: {$gt: 1}}, {x: {$lt: 3}} ] }"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        // TODO check filter

        QuerySolution* indexedSolution;
        // This is a fetch not an ixscan because our index tagging isn't good so far and we don't
        // know that the index is used for the second predicate.
        getPlanByType(STAGE_FETCH, &indexedSolution);
        cout << indexedSolution->toString() << endl;

        //FetchNode* fn = static_cast<FetchNode*>(indexedSolution->root.get());
        //IndexScanNode* ixNode = static_cast<IndexScanNode*>(fn->child.get());
        //boundsEqual(BSON("x" << BSON_ARRAY(BSON_ARRAY(1 << MAXKEY << false << true))), ixNode->bounds);
        // TODO: use this when we tag both indices.
        // boundsEqual(fromjson("{x: [ [1, 3, false, false] ] }"), ixNode->bounds);
        // TODO check filter
    }

    //
    // Basic covering
    //

    TEST_F(IndexAssignmentTest, BasicCovering) {
        addIndex(BSON("x" << 1));
        // query, sort, proj
        runDetailedQuery(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{_id: 0, x: 1}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        vector<QuerySolution*> solns;
        getAllPlans(STAGE_PROJECTION, &solns);
        ASSERT_EQUALS(solns.size(), 2U);

        for (size_t i = 0; i < solns.size(); ++i) {
            cout << solns[i]->toString();
            ProjectionNode* pn = static_cast<ProjectionNode*>(solns[i]->root.get());
            ASSERT(STAGE_COLLSCAN == pn->children[0]->getType() || STAGE_IXSCAN == pn->children[0]->getType());
        }
    }

    //
    // Basic sort elimination
    //

    TEST_F(IndexAssignmentTest, BasicSortElim) {
        addIndex(BSON("x" << 1));
        // query, sort, proj
        runDetailedQuery(fromjson("{ x : {$gt: 1}}"), fromjson("{x: 1}"), BSONObj());
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_SORT, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    //
    // Basic compound
    //

    TEST_F(IndexAssignmentTest, BasicCompound) {
        addIndex(BSON("x" << 1 << "y" << 1));
        runQuery(fromjson("{ x : 5, y: 10}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    TEST_F(IndexAssignmentTest, CompoundMissingField) {
        addIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
        runQuery(fromjson("{ x : 5, z: 10}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    TEST_F(IndexAssignmentTest, CompoundFieldsOrder) {
        addIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
        runQuery(fromjson("{ x : 5, z: 10, y:1}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    TEST_F(IndexAssignmentTest, CantUseCompound) {
        addIndex(BSON("x" << 1 << "y" << 1));
        runQuery(fromjson("{ y: 10}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
    }

    //
    // Array operators
    //

    TEST_F(IndexAssignmentTest, ElemMatchOneField) {
        addIndex(BSON("a.b" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    TEST_F(IndexAssignmentTest, ElemMatchTwoFields) {
        addIndex(BSON("a.b" << 1));
        addIndex(BSON("a.c" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 3U);
    }

    TEST_F(IndexAssignmentTest, BasicAllElemMatch) {
        addIndex(BSON("foo.a" << 1));
        addIndex(BSON("foo.b" << 1));
        runQuery(fromjson("{foo: {$all: [ {$elemMatch: {a:1, b:1}}, {$elemMatch: {a:2, b:2}}]}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 5U);
    }

    TEST_F(IndexAssignmentTest, ElemMatchValueMatch) {
        addIndex(BSON("foo" << 1));
        addIndex(BSON("foo" << 1 << "bar" << 1));
        runQuery(fromjson("{foo: {$elemMatch: {$gt: 5, $lt: 10}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 3U);
    }

    TEST_F(IndexAssignmentTest, ElemMatchNested) {
        addIndex(BSON("a.b.c" << 1));
        runQuery(fromjson("{ a:{ $elemMatch:{ b:{ $elemMatch:{ c:{ $gte:1, $lte:1 } } } } }}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    TEST_F(IndexAssignmentTest, TwoElemMatchNested) {
        addIndex(BSON("a.d.e" << 1));
        addIndex(BSON("a.b.c" << 1));
        runQuery(fromjson("  { a:{ $elemMatch:{ d:{ $elemMatch:{ e:{ $lte:1 } } },"
                                               "b:{ $elemMatch:{ c:{ $gte:1 } } } } } }"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 3U);
    }

    TEST_F(IndexAssignmentTest, ElemMatchCompoundTwoFields) {
        addIndex(BSON("a.b" << 1 << "a.c" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
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
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);

        // Center
        runQuery(fromjson("{a : { $within : { $center : [[ 5, 5 ], 7 ] } }}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);

        // Centersphere
        runQuery(fromjson("{a : { $within : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);

        // Within box.
        runQuery(fromjson("{a : {$within: {$box : [[0,0],[9,9]]}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);

        // TODO: test that we *don't* annotate for things we shouldn't.
    }

    TEST_F(IndexAssignmentTest, Basic2DSphereNonNear) {
        // 2dsphere can do: within+geometry, intersects+geometry
        addIndex(BSON("a" << "2dsphere"));

        runQuery(fromjson("{a: {$geoIntersects: {$geometry: {type: 'Point',"
                                                           "coordinates: [10.0, 10.0]}}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);

        runQuery(fromjson("{a : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);

        // TODO: test that we *don't* annotate for things we shouldn't.
    }

    TEST_F(IndexAssignmentTest, Basic2DGeoNear) {
        // Can only do near + old point.
        addIndex(BSON("a" << "2d"));
        runQuery(fromjson("{a: {$near: [0,0], $maxDistance:0.3 }}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 1U);
    }

    TEST_F(IndexAssignmentTest, Basic2DSphereGeoNear) {
        // Can do nearSphere + old point, near + new point.
        addIndex(BSON("a" << "2dsphere"));

        runQuery(fromjson("{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 1U);

        runQuery(fromjson("{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
                                          "$maxDistance:100}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 1U);
    }

    TEST_F(IndexAssignmentTest, Basic2DSphereGeoNearReverseCompound) {
        addIndex(BSON("x" << 1));
        addIndex(BSON("x" << 1 << "a" << "2dsphere"));
        runQuery(fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 1U);
    }

    TEST_F(IndexAssignmentTest, NearNoIndex) {
        addIndex(BSON("x" << 1));
        runQuery(fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 0U);
    }

    TEST_F(IndexAssignmentTest, TwoDSphereNoGeoPred) {
        addIndex(BSON("x" << 1 << "a" << "2dsphere"));
        runQuery(fromjson("{x:1}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    //
    // Multiple solutions
    //

    TEST_F(IndexAssignmentTest, TwoPlans) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("a" << 1 << "b" << 1));

        runQuery(fromjson("{a:1, b:{$gt:2,$lt:2}}"));

        dumpSolutions();

        // 2 indexed solns and one non-indexed
        ASSERT_EQUALS(getNumSolutions(), 3U);
    }

    TEST_F(IndexAssignmentTest, TwoPlansElemMatch) {
        addIndex(BSON("a" << 1 << "b" << 1));
        addIndex(BSON("arr.x" << 1 << "a" << 1));

        runQuery(fromjson("{arr: { $elemMatch : { x : 5 , y : 5 } },"
                          " a : 55 , b : { $in : [ 1 , 5 , 8 ] } }"));

        dumpSolutions();

        // 2 indexed solns and one non-indexed
        ASSERT_EQUALS(getNumSolutions(), 3U);
    }

    //
    // Sort orders
    //

    // SERVER-1205.
    TEST_F(IndexAssignmentTest, MergeSort) {
        addIndex(BSON("a" << 1 << "c" << 1));
        addIndex(BSON("b" << 1 << "c" << 1));
        runDetailedQuery(fromjson("{$or: [{a:1}, {b:1}]}"), fromjson("{c:1}"), BSONObj());
        dumpSolutions();
    }

    // SERVER-1205 as well.
    TEST_F(IndexAssignmentTest, NoMergeSortIfNoSortWanted) {
        addIndex(BSON("a" << 1 << "c" << 1));
        addIndex(BSON("b" << 1 << "c" << 1));
        runDetailedQuery(fromjson("{$or: [{a:1}, {b:1}]}"), BSONObj(), BSONObj());
        dumpSolutions();
    }

    // SERVER-10801
    TEST_F(IndexAssignmentTest, SortOnGeoQuery) {
        addIndex(BSON("timestamp" << -1 << "position" << "2dsphere"));
        BSONObj query = fromjson("{position: {$geoWithin: {$geometry: {type: \"Polygon\", coordinates: [[[1, 1], [1, 90], [180, 90], [180, 1], [1, 1]]]}}}}");
        BSONObj sort = fromjson("{timestamp: -1}");
        runDetailedQuery(query, sort, BSONObj());
        dumpSolutions();
    }

    // SERVER-9257
    TEST_F(IndexAssignmentTest, CompoundGeoNoGeoPredicate) {
        addIndex(BSON("creationDate" << 1 << "foo.bar" << "2dsphere"));
        runDetailedQuery(fromjson("{creationDate: { $gt: 7}}"),
                         fromjson("{creationDate: 1}"), BSONObj());
        dumpSolutions();
    }

    // Basic "keep sort in mind with an OR"
    TEST_F(IndexAssignmentTest, MergeSortEvenIfSameIndex) {
        addIndex(BSON("a" << 1 << "b" << 1));
        runDetailedQuery(fromjson("{$or: [{a:1}, {a:7}]}"), fromjson("{b:1}"), BSONObj());
        dumpSolutions();
    }

    TEST_F(IndexAssignmentTest, ReverseScanForSort) {
        addIndex(BSON("_id" << 1));
        runDetailedQuery(BSONObj(), fromjson("{_id: -1}"), BSONObj());
        dumpSolutions();
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

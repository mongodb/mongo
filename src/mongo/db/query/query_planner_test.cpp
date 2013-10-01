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
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

    static const char* ns = "somebogusns";

    class SingleIndexTest : public mongo::unittest::Test {
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

        void setIndex(BSONObj keyPattern) {
            // false means not multikey.
            keyPatterns.push_back(IndexEntry(keyPattern, false));
        }

        //
        // Execute planner.
        //

        void runQuery(BSONObj query) {
            queryObj = query.getOwned();
            ASSERT_OK(CanonicalQuery::canonicalize(ns, queryObj, &cq));
            QueryPlanner::plan(*cq, keyPatterns, &solns);
            ASSERT_GREATER_THAN(solns.size(), 0U);;
        }

        void runDetailedQuery(const BSONObj& query, const BSONObj& sort, const BSONObj& proj) {
            ASSERT_OK(CanonicalQuery::canonicalize(ns, query, sort, proj, &cq));
            QueryPlanner::plan(*cq, keyPatterns, &solns);
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
        vector<IndexEntry> keyPatterns;
        vector<QuerySolution*> solns;
    };

    //
    // Equality
    //

    TEST_F(SingleIndexTest, EqualityIndexScan) {
        setIndex(BSON("x" << 1));
        runQuery(BSON("x" << 5));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        // TODO check filter

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        FetchNode* fn = static_cast<FetchNode*>(indexedSolution->root.get());
        IndexScanNode* ixNode = static_cast<IndexScanNode*>(fn->child.get());

        boundsEqual(fromjson("{x: [ [5, 5, true, true] ] }"), ixNode->bounds);
        // TODO check filter
    }

    TEST_F(SingleIndexTest, EqualityIndexScanWithTrailingFields) {
        setIndex(BSON("x" << 1 << "y" << 1));
        runQuery(BSON("x" << 5));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        // TODO check filter

        //QuerySolution* indexedSolution;
        //getPlanByType(STAGE_FETCH, &indexedSolution);
        //FetchNode* fn = static_cast<FetchNode*>(indexedSolution->root.get());
        //IndexScanNode* ixNode = static_cast<IndexScanNode*>(fn->child.get());

        // XXX: this boundsEqual check is bogus, need bounds on y.
        // boundsEqual(fromjson("{x: [ [5, 5, true, true] ] }"), ixNode->bounds);

        // TODO check filter
    }

    // TODO: Check compound indices.
    // TODO: Check diff. index directions for ranges below.

    //
    // <
    //

    TEST_F(SingleIndexTest, LessThan) {
        setIndex(BSON("x" << 1));
        runQuery(BSON("x" << BSON("$lt" << 5)));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        // TODO check filter

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);

        //FetchNode* fn = static_cast<FetchNode*>(indexedSolution->root.get());
        //IndexScanNode* ixNode = static_cast<IndexScanNode*>(fn->child.get());
        //BSONObj bounds = BSON("x" << BSON_ARRAY(BSON_ARRAY(MINKEY << 5 << true << false)));
        //boundsEqual(bounds, ixNode->bounds);
        // todo check filter
    }

    //
    // <=
    //

    TEST_F(SingleIndexTest, LessThanEqual) {
        setIndex(BSON("x" << 1));
        runQuery(BSON("x" << BSON("$lte" << 5)));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        // TODO check filter

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);

        //FetchNode* fn = static_cast<FetchNode*>(indexedSolution->root.get());
        //IndexScanNode* ixNode = static_cast<IndexScanNode*>(fn->child.get());

        //BSONObj bounds = BSON("x" << BSON_ARRAY(BSON_ARRAY(MINKEY << 5 << true << true)));
        //boundsEqual(bounds, ixNode->bounds);
        // todo check filter
    }

    //
    // >
    //

    TEST_F(SingleIndexTest, GreaterThan) {
        setIndex(BSON("x" << 1));
        runQuery(BSON("x" << BSON("$gt" << 5)));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        // TODO check filter

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);

        //FetchNode* fn = static_cast<FetchNode*>(indexedSolution->root.get());
        //IndexScanNode* ixNode = static_cast<IndexScanNode*>(fn->child.get());
        //BSONObj bounds = BSON("x" << BSON_ARRAY(BSON_ARRAY(5 << MAXKEY << false << true)));
        //boundsEqual(bounds, ixNode->bounds);
        // todo check filter
    }

    //
    // >=
    //

    TEST_F(SingleIndexTest, GreaterThanEqual) {
        setIndex(BSON("x" << 1));
        runQuery(BSON("x" << BSON("$gte" << 5)));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        // TODO check filter

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);

        //FetchNode* fn = static_cast<FetchNode*>(indexedSolution->root.get());
        //IndexScanNode* ixNode = static_cast<IndexScanNode*>(fn->child.get());
        //BSONObj bounds = BSON("x" << BSON_ARRAY(BSON_ARRAY(5 << MAXKEY << true << true)));
        //boundsEqual(bounds, ixNode->bounds);
        // todo check filter
    }

    //
    // tree operations
    //

    TEST_F(SingleIndexTest, TwoPredicatesAnding) {
        setIndex(BSON("x" << 1));
        runQuery(fromjson("{$and: [ {x: {$gt: 1}}, {x: {$lt: 3}} ] }"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        // TODO check filter

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

    TEST_F(SingleIndexTest, SimpleOr) {
        setIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [ {a: 20}, {a: 21}]}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        QuerySolution* indexedSolution = NULL;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        cout << indexedSolution->toString() << endl;
    }

    TEST_F(SingleIndexTest, OrWithoutEnoughIndices) {
        setIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [ {a: 20}, {b: 21}]}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
        cout << collScanSolution->toString() << endl;
    }

    TEST_F(SingleIndexTest, OrWithAndChild) {
        setIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [ {a: 20}, {$and: [{a:1}, {b:7}]}]}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        QuerySolution* indexedSolution = NULL;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        cout << indexedSolution->toString() << endl;
    }

    TEST_F(SingleIndexTest, AndWithUnindexedOrChild) {
        setIndex(BSON("a" << 1));
        runQuery(fromjson("{a:20, $or: [{b:1}, {c:7}]}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        QuerySolution* indexedSolution = NULL;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        cout << indexedSolution->toString() << endl;
    }


    TEST_F(SingleIndexTest, AndWithOrWithOneIndex) {
        setIndex(BSON("b" << 1));
        setIndex(BSON("a" << 1));
        runQuery(fromjson("{$or: [{b:1}, {c:7}], a:20}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);
        QuerySolution* indexedSolution = NULL;
        getPlanByType(STAGE_FETCH, &indexedSolution);
        cout << indexedSolution->toString() << endl;
    }

    //
    // Tree operations that require simple tree rewriting.
    //

    TEST_F(SingleIndexTest, AndOfAnd) {
        setIndex(BSON("x" << 1));
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

    TEST_F(SingleIndexTest, BasicCovering) {
        setIndex(BSON("x" << 1));
        // query, sort, proj
        runDetailedQuery(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{x: 1}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        vector<QuerySolution*> solns;
        getAllPlans(STAGE_PROJECTION, &solns);
        ASSERT_EQUALS(solns.size(), 2U);

        for (size_t i = 0; i < solns.size(); ++i) {
            // cout << solns[i]->toString();
            ProjectionNode* pn = static_cast<ProjectionNode*>(solns[i]->root.get());
            ASSERT(STAGE_COLLSCAN == pn->child->getType() || STAGE_IXSCAN == pn->child->getType());
        }
    }

    //
    // Basic sort elimination
    //

    TEST_F(SingleIndexTest, BasicSortElim) {
        setIndex(BSON("x" << 1));
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

    TEST_F(SingleIndexTest, BasicCompound) {
        setIndex(BSON("x" << 1 << "y" << 1));
        runQuery(fromjson("{ x : 5, y: 10}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    TEST_F(SingleIndexTest, CompoundMissingField) {
        setIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
        runQuery(fromjson("{ x : 5, z: 10}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    TEST_F(SingleIndexTest, CompoundFieldsOrder) {
        setIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
        runQuery(fromjson("{ x : 5, z: 10, y:1}"));
        ASSERT_EQUALS(getNumSolutions(), 2U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_FETCH, &indexedSolution);
    }

    TEST_F(SingleIndexTest, CantUseCompound) {
        setIndex(BSON("x" << 1 << "y" << 1));
        runQuery(fromjson("{ y: 10}"));
        ASSERT_EQUALS(getNumSolutions(), 1U);

        QuerySolution* collScanSolution;
        getPlanByType(STAGE_COLLSCAN, &collScanSolution);
    }

    //
    // Array operators
    //

    TEST_F(SingleIndexTest, ElemMatchOneField) {
        setIndex(BSON("a.b" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    TEST_F(SingleIndexTest, ElemMatchTwoFields) {
        setIndex(BSON("a.b" << 1));
        setIndex(BSON("a.c" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    TEST_F(SingleIndexTest, BasicAllElemMatch) {
        setIndex(BSON("foo.a" << 1));
        setIndex(BSON("foo.b" << 1));
        runQuery(fromjson("{foo: {$all: [ {$elemMatch: {a:1, b:1}}, {$elemMatch: {a:2, b:2}}]}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    TEST_F(SingleIndexTest, ElemMatchValueMatch) {
        setIndex(BSON("foo" << 1));
        runQuery(fromjson("{foo: {$elemMatch: {$gt: 5, $lt: 10}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    TEST_F(SingleIndexTest, ElemMatchNested) {
        setIndex(BSON("a.b.c" << 1));
        runQuery(fromjson("{ a:{ $elemMatch:{ b:{ $elemMatch:{ c:{ $gte:1, $lte:1 } } } } }}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    TEST_F(SingleIndexTest, TwoElemMatchNested) {
        setIndex(BSON("a.d.e" << 1));
        setIndex(BSON("a.b.c" << 1));
        runQuery(fromjson("  { a:{ $elemMatch:{ d:{ $elemMatch:{ e:{ $lte:1 } } },"
                                               "b:{ $elemMatch:{ c:{ $gte:1 } } } } } }"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    TEST_F(SingleIndexTest, ElemMatchCompoundTwoFields) {
        setIndex(BSON("a.b" << 1 << "a.c" << 1));
        runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));
        dumpSolutions();
        ASSERT_EQUALS(getNumSolutions(), 2U);
    }

    // STOPPED HERE - need to hook up machinery for multiple indexed predicates
    //                second is not working (until the machinery is in place)
    //
    // TEST_F(SingleIndexTest, TwoPredicatesOring) {
    //     setIndex(BSON("x" << 1));
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

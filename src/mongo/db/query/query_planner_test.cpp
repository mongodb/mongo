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
            keyPatterns.push_back(keyPattern);
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

        //
        // Introspect solutions.
        //

        size_t getNumSolutions() const {
            return solns.size();
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
            ASSERT_EQUALS(found, 1U);
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
        vector<BSONObj> keyPatterns;
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
        getPlanByType(STAGE_IXSCAN, &indexedSolution);
        IndexScanNode* ixNode = static_cast<IndexScanNode*>(indexedSolution->root.get());
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

        QuerySolution* indexedSolution;
        getPlanByType(STAGE_IXSCAN, &indexedSolution);
        IndexScanNode* ixNode = static_cast<IndexScanNode*>(indexedSolution->root.get());
        boundsEqual(fromjson("{x: [ [5, 5, true, true] ] }"), ixNode->bounds);
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
        getPlanByType(STAGE_IXSCAN, &indexedSolution);
        IndexScanNode* ixNode = static_cast<IndexScanNode*>(indexedSolution->root.get());
        BSONObj bounds = BSON("x" << BSON_ARRAY(BSON_ARRAY(MINKEY << 5 << true << false)));
        boundsEqual(bounds, ixNode->bounds);
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
        getPlanByType(STAGE_IXSCAN, &indexedSolution);
        IndexScanNode* ixNode = static_cast<IndexScanNode*>(indexedSolution->root.get());
        BSONObj bounds = BSON("x" << BSON_ARRAY(BSON_ARRAY(MINKEY << 5 << true << true)));
        boundsEqual(bounds, ixNode->bounds);
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
        getPlanByType(STAGE_IXSCAN, &indexedSolution);
        IndexScanNode* ixNode = static_cast<IndexScanNode*>(indexedSolution->root.get());
        BSONObj bounds = BSON("x" << BSON_ARRAY(BSON_ARRAY(5 << MAXKEY << false << true)));
        boundsEqual(bounds, ixNode->bounds);
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
        getPlanByType(STAGE_IXSCAN, &indexedSolution);
        IndexScanNode* ixNode = static_cast<IndexScanNode*>(indexedSolution->root.get());
        BSONObj bounds = BSON("x" << BSON_ARRAY(BSON_ARRAY(5 << MAXKEY << true << true)));
        boundsEqual(bounds, ixNode->bounds);
        // todo check filter
    }

    //
    // tree operations
    //

    // STOPPED HERE - need to hook up machinery for multiple indexed predicates
    //                first test is segfaulting
    //                second is not working (until the machinery is in place)
    //
    // TEST_F(SingleIndexTest, TwoPredicatesAnding) {
    //     setIndex(BSON("x" << 1));
    //     runQuery(fromjson("{$and: [ {$gt: 1}, {$lt: 3} ] }"));
    //     ASSERT_EQUALS(getNumSolutions(), 2U);

    //     QuerySolution* collScanSolution;
    //     getPlanByType(STAGE_COLLSCAN, &collScanSolution);
    //     // TODO check filter

    //     QuerySolution* indexedSolution;
    //     getPlanByType(STAGE_IXSCAN, &indexedSolution);
    //     IndexScanNode* ixNode = static_cast<IndexScanNode*>(indexedSolution->root.get());
    //     boundsEqual(fromjson("{x: [ [1, 3, false, false] ] }"), ixNode->bounds);
    //     // TODO check filter
    // }

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

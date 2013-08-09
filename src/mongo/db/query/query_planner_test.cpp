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
 */

/**
 * This file contains tests for mongo/db/query/query_planner.cpp
 */

#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

    static const char* ns = "somebogusns";

    // Test the most basic index-using functionality.
    TEST(QueryPlannerTest, EqualityIndexScan) {
        // Get the canonical query.
        BSONObj queryObj = BSON("x" << 5);
        CanonicalQuery* cq;
        ASSERT(CanonicalQuery::canonicalize(ns, queryObj, &cq).isOK());

        // This is our index.  It's the simplest index we can use.
        BSONObjSet indices;
        indices.insert(BSON("x" << 1));

        // If we can't get this right...
        vector<QuerySolution*> solns;
        QueryPlanner::planWithIndices(*cq, indices, &solns);
        ASSERT_EQUALS(size_t(1), solns.size());

        QuerySolutionNode* node = solns[0]->root.get();
        ASSERT_EQUALS(STAGE_IXSCAN, node->getType());

        // TODO: Check bounds.
    }

    TEST(QueryPlannerTest, EqualityIndexScanWithTrailingFields) {
        BSONObj queryObj = BSON("x" << 5);

        CanonicalQuery* cq;
        ASSERT(CanonicalQuery::canonicalize(ns, queryObj, &cq).isOK());

        BSONObjSet indices;
        indices.insert(BSON("x" << 1 << "y" << 1));

        vector<QuerySolution*> solns;
        QueryPlanner::planWithIndices(*cq, indices, &solns);

        // Available index is prefixed by our equality, use it.
        ASSERT_EQUALS(size_t(1), solns.size());

        QuerySolutionNode* node = solns[0]->root.get();
        ASSERT_EQUALS(STAGE_IXSCAN, node->getType());

        // TODO: Check bounds.
    }

}  // namespace

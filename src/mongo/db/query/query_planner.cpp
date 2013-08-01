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

#include "mongo/db/query/query_planner.h"

#include "mongo/db/query/query_solution.h"
#include "mongo/db/matcher/expression_parser.h"

namespace mongo {

    // static
    void QueryPlanner::plan(const CanonicalQuery& query, vector<QuerySolution*> *out) {
        const MatchExpression* root = query.root();

        // The default plan is always a collection scan with a heavy filter.  This is a valid
        // solution for any query that does not require an index.
        if (!requiresIndex(root)) {
            auto_ptr<QuerySolution> soln(new QuerySolution());
            soln->filterData = query.getQueryObj();
            // TODO: have a MatchExpression::copy function(?)
            StatusWithMatchExpression swme = MatchExpressionParser::parse(soln->filterData);
            verify(swme.isOK());
            soln->filter.reset(swme.getValue());

            // Make the (only) node, a collection scan.
            CollectionScanNode* csn = new CollectionScanNode();
            csn->name = query.ns();
            csn->filter = soln->filter.get();

            // Add this solution to the list of solutions.
            soln->root.reset(csn);
            out->push_back(soln.release());

            // TODO: limit and skip
            // TODO: sort.
        }
    }

    // static 
    bool QueryPlanner::requiresIndex(const MatchExpression* node) {
        if (MatchExpression::GEO_NEAR == node->matchType()) {
            return true;
        }

        for (size_t i = 0; i < node->numChildren(); ++i) {
            if (requiresIndex(node->getChild(i))) {
                return true;
            }
        }

        return false;
    }

}  // namespace mongo

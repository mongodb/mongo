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

#pragma once

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/predicate_map.h"

namespace mongo {

    /**
     * Provides elements from the power set of possible indices to use.  Uses the available
     * predicate information to make better decisions about what indices are best.
     *
     * TODO: Use stats about indices.
     */
    class PlanEnumerator {
        MONGO_DISALLOW_COPYING(PlanEnumerator);
    public:
        /**
         * Constructs an enumerator for the query specified in 'root', given that the predicates
         * that can be solved using indices are listed at 'pm', and the index patterns
         * mentioned there are described by 'indices'.
         *
         * Does not take ownership of any arguments.  They must outlive any calls to getNext(...).
         */
        PlanEnumerator(MatchExpression* root,
                       const PredicateMap* pm,
                       const vector<BSONObj>* indices);

        /**
         * Returns OK and performs a sanity check on the input parameters and prepares the
         * internal state so that getNext() can be called. Returns an error status with a
         * description if the sanity check failed.
         */
        Status init();

        /**
         * Outputs a possible plan.  Leaves in the plan are tagged with an index to use.
         * Returns true if a plan was outputted, false if no more plans will be outputted.
         *
         * 'tree' is set to point to the query tree.  A QuerySolution is built from this tree.
         * Caller owns the pointer.  Note that 'tree' itself points into data owned by the
         * provided CanonicalQuery.
         *
         * Nodes in 'tree' are tagged with indices that should be used to answer the tagged nodes.
         * Only nodes that have a field name (isLogical() == false) will be tagged.
         */
        bool getNext(MatchExpression** tree);

    private:
        // Match expression we're planning for. Not owned by us.
        MatchExpression* _root;

        // A map from a field name into the nodes of the match expression that can be solved
        // using indices (and which ones). Not owned by us.
        const PredicateMap& _pm;

        // Index pattern of the indices mentined in '_pm'. Not owned by us.
        const std::vector<BSONObj>& _indices;

        //
        // navigation state (work in progress)
        //
        // We intend to enumerate, initially, solely based on the distinct index access
        // patterns that a query would accept. The enumeration state, below, will change as we
        // progress toward that goal. For now, we simplify by imposing the following
        // assumptions
        //
        // + Each index can help only one predicate in a query
        // + There is only one index that can help a query
        //
        // TODO: Relax the above
        //

        // List of all the useful indices and which node in the match expression each of them
        // applies to.
        struct IndexInfo{
            int index;
            MatchExpression* node;
        };
        vector<IndexInfo> _indexes;

        // Iterator over _indices. Points to the next index to be used when getNext is called.
        size_t _iter;
    };

} // namespace mongo

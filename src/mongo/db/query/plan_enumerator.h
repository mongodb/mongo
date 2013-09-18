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

        //
        // Data used by all enumeration strategies
        //

        // Match expression we're planning for. Not owned by us.
        MatchExpression* _root;

        // A map from a field name into the nodes of the match expression that can be solved
        // using indices (and which ones). Not owned by us.
        const PredicateMap& _pm;

        // Index pattern of the indices mentined in '_pm'. Not owned by us.
        const std::vector<BSONObj>& _indices;

        //
        // Enumeration Strategies
        //

        //
        // Legacy strategy.
        //
        // The legacy strategy assigns the absolute fewest number of indices require to satisfy a
        // query.  Some predicates require an index (GEO_NEAR and TEXT).  Each branch of an OR requires
        // an index.
        //

        // Which leaves require an index?
        vector<MatchExpression*> _leavesRequireIndex;

        // For each leaf, a counter of which index we've assigned so far.
        vector<size_t> _assignedCounter;

        // Are we done with the legacy strategy?
        bool _done;

        /**
         * Fill out _leavesRequireIndex such that each OR clause and each index-requiring leaf has
         * an index.  If there are no OR clauses, we use only one index.
         */
        bool prepLegacyStrategy(MatchExpression* root);

        /**
         * Does the provided node have any indices that can be used to answer it?
         */
        bool hasIndexAvailable(MatchExpression* node);

        // XXX TODO: Add a dump() or toString() for legacy strategy.
    };

} // namespace mongo

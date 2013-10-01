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
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/index_tag.h"

namespace mongo {

    /**
     * Provides elements from the power set of possible indices to use.  Uses the available
     * predicate information to make better decisions about what indices are best.
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
        PlanEnumerator(MatchExpression* root, const vector<IndexEntry>* indices);
        ~PlanEnumerator();

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

        // Indices we're allowed to enumerate with.
        const vector<IndexEntry>* _indices;

        //
        // Memoization strategy
        //

        /**
         * Traverses the match expression and generates the memo structure from it.
         * Returns true if the provided node uses an index, false otherwise.
         */
        bool prepMemo(MatchExpression* node);

        /**
         * Returns true if index #idx is compound, false otherwise.
         */
        bool isCompound(size_t idx);

        /**
         * When we assign indices to nodes, we only assign indices to predicates that are 'first'
         * indices (the predicate is over the first field in the index).
         *
         * If an assigned index is compound, checkCompound looks for predicates that are over fields
         * in the compound index.
         */
        void checkCompound(string prefix, MatchExpression* node);

        /**
         * Traverses the memo structure and annotates the tree with IndexTags for the chosen
         * indices.
         */
        void tagMemo(size_t id);

        /**
         * Move to the next enumeration state.  Enumeration state is stored in curEnum.
         *
         * Returns true if the memo subtree with root 'node' has no further enumeration states.  In this
         * case, that subtree restarts its enumeration at the beginning state.  This implies that
         * the parent of node should move to the next state.  If 'node' is the root of the tree,
         * we are done with enumeration.
         *
         * Returns false if the memo subtree has moved to the next state.
         *
         * XXX implement.
         */
        bool nextMemo(size_t id);

        struct PredicateSolution {
            vector<size_t> first;
            vector<size_t> notFirst;
            // Not owned here.
            MatchExpression* expr;
        };

        struct AndSolution {
            // Must use one of the elements of subnodes.
            vector<vector<size_t> > subnodes;
        };

        struct OrSolution {
            // Must use all of subnodes.
            vector<size_t> subnodes;
        };

        struct NodeSolution {
            scoped_ptr<PredicateSolution> pred;
            scoped_ptr<AndSolution> andSolution;
            scoped_ptr<OrSolution> orSolution;
            string toString() const;
        };

        // Memoization

        // Used to label nodes in the order in which we visit in a post-order traversal.
        size_t _inOrderCount;

        // Map from node to its order/ID.
        map<MatchExpression*, size_t> _nodeToId;

        // Map from order/ID to a memoized solution.
        map<size_t, NodeSolution*> _memo;

        // Enumeration

        // ANDs count through clauses, PREDs count through indices.
        // Index is order/ID.
        // Value is whatever counter that node needs.
        map<size_t, size_t> _curEnum;

        // If true, there are no further enumeration states, and getNext should return false.
        // We could be _done immediately after init if we're unable to output an indexed plan.
        bool _done;
    };

} // namespace mongo

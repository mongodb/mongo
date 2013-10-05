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
         * Constructs an enumerator for the query specified in 'root' which is tagged with
         * RelevantTag(s).  The index patterns mentioned in the tags are described by 'indices'.
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
         * 'tree' is set to point to the query tree.  A QueryAssignment is built from this tree.
         * Caller owns the pointer.  Note that 'tree' itself points into data owned by the
         * provided CanonicalQuery.
         *
         * Nodes in 'tree' are tagged with indices that should be used to answer the tagged nodes.
         * Only nodes that have a field name (isLogical() == false) will be tagged.
         */
        bool getNext(MatchExpression** tree);

    private:

        //
        // Memoization strategy
        //


        // Everything is really a size_t but it's far more readable to impose a type via typedef.

        // An ID we use to index into _memo.  An entry in _memo is a NodeAssignment.
        typedef size_t NodeID;

        // An index in _indices.
        typedef size_t IndexID;

        // The position of a field in a possibly compound index.
        typedef size_t IndexPosition;

        /**
         * Traverses the match expression and generates the memo structure from it.
         * Returns true if the provided node uses an index, false otherwise.
         */
        bool prepMemo(MatchExpression* node);

        /**
         * Returns true if index #idx is compound, false otherwise.
         */
        bool isCompound(IndexID idx);

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
        void tagMemo(NodeID id);

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
        bool nextMemo(NodeID id);

        struct PredicateAssignment {
            vector<IndexID> first;
            vector<IndexID> notFirst;
            // Not owned here.
            MatchExpression* expr;
        };

        struct OrAssignment {
            // Must use all of subnodes.
            vector<NodeID> subnodes;
        };

        // This is used by NewAndAssignment, not an actual assignment.
        struct OneIndexAssignment {
            // 'preds[i]' is uses index 'index' at position 'positions[i]'
            vector<MatchExpression*> preds;
            vector<IndexPosition> positions;
            IndexID index;
        };

        struct NewAndAssignment {
            // TODO: We really want to consider the power set of the union of the choices here.
            vector<OneIndexAssignment> predChoices;
            vector<NodeID> subnodes;
        };

        struct NodeAssignment {
            scoped_ptr<PredicateAssignment> pred;
            scoped_ptr<OrAssignment> orAssignment;
            scoped_ptr<NewAndAssignment> newAnd;
            string toString() const;
        };

        // Memoization

        // Used to label nodes in the order in which we visit in a post-order traversal.
        size_t _inOrderCount;

        // Map from expression to its NodeID.
        unordered_map<MatchExpression*, NodeID> _nodeToId;

        // Map from NodeID to its precomputed solution info.
        unordered_map<NodeID, NodeAssignment*> _memo;

        // Enumeration

        // Map from NodeID to a counter that the NodeID uses to enumerate its states.
        unordered_map<NodeID, size_t> _curEnum;

        // If true, there are no further enumeration states, and getNext should return false.
        // We could be _done immediately after init if we're unable to output an indexed plan.
        bool _done;

        //
        // Data used by all enumeration strategies
        //

        // Match expression we're planning for. Not owned by us.
        MatchExpression* _root;

        // Indices we're allowed to enumerate with.
        const vector<IndexEntry>* _indices;
    };

} // namespace mongo

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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
        typedef size_t MemoID;

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
        void tagMemo(MemoID id);

        /**
         * Move to the next enumeration state.  Each assignment stores its own enumeration state.
         * See the various ____Assignment classes below for details on enumeration state.
         *
         * Returns true if the memo subtree with root 'node' has no further enumeration states.  In
         * this case, that subtree restarts its enumeration at the beginning state.  This implies
         * that the parent of node should move to the next state.  If 'node' is the root of the
         * tree, we are done with enumeration.
         *
         * The return of this function can be thought of like a 'carry' in addition.
         *
         * Returns false if the memo subtree has moved to the next state.
         */
        bool nextMemo(MemoID id);

        /**
         * A short word on the memo structure.
         *
         * The PlanEnumerator is interested in matching predicates and indices.  Predicates
         * are leaf nodes in the parse tree.  {x:5}, {x: {$geoWithin:...}} are both predicates.
         *
         * When we have simple predicates, like {x:5}, the task is easy: any indices prefixed
         * with 'x' can be used to answer the predicate.  This is where the PredicateAssignment
         * is used.
         *
         * With logical operators, things are more complicated.  Let's start with OR, the simplest.
         * Since the output of an OR is the union of its results, each of its children must be
         * indexed for the entire OR to be indexed.  If each subtree of an OR is indexable, the
         * OR is as well.
         *
         * For an AND to be indexed, only one of its children must be indexed.  AND is an
         * intersection of its children, so each of its children describes a superset of the
         * produced results.
         */

        struct PredicateAssignment {
            PredicateAssignment() : indexToAssign(0) { }

            vector<IndexID> first;
            // Not owned here.
            MatchExpression* expr;

            // Enumeration state.  An indexed predicate's possible states are the indices that the
            // predicate can directly use (the 'first' indices).  As such this value ranges from 0
            // to first.size()-1 inclusive.
            size_t indexToAssign;
        };

        struct OrAssignment {
            // Must use all of subnodes.
            vector<MemoID> subnodes;

            // No enumeration state.  Each child of an OR must be indexed for the OR to be indexed.
            // When an OR moves to a subsequent state it just asks all its children to move their
            // states forward.
        };

        // This is used by AndAssignment and is not an actual assignment.
        struct OneIndexAssignment {
            // 'preds[i]' is uses index 'index' at position 'positions[i]'
            vector<MatchExpression*> preds;
            vector<IndexPosition> positions;
            IndexID index;
        };

        struct AndAssignment {
            // Enumeration state
            enum EnumerationState {
                // First this
                MANDATORY,
                // Then this
                PRED_CHOICES,
                // Then this
                SUBNODES,
                // Then we have a carry and back to MANDATORY.
            };

            AndAssignment() : state(MANDATORY), counter(0) { }

            // These index assignments must exist in every choice we make (GEO_NEAR and TEXT).
            vector<OneIndexAssignment> mandatory;
            // TODO: We really want to consider the power set of the union of predChoices, subnodes.
            vector<OneIndexAssignment> predChoices;
            vector<MemoID> subnodes;

            // In the simplest case, an AndAssignment picks indices like a PredicateAssignment.  To
            // be indexed we must only pick one index, which is currently what is done.
            //
            // Complications:
            //
            // Some of our child predicates cannot be answered without an index.  As such, the
            // indices that those predicates require must always be outputted.  We store these
            // mandatory index assignments in 'mandatory'.
            //
            // Some of our children may not be predicates.  We may have ORs (or array operators) as
            // children.  If one of these subtrees provides an index, the AND is indexed.  We store
            // these subtree choices in 'subnodes'.
            //
            // With the above two cases out of the way, we can focus on the remaining case: what to
            // do with our children that are leaf predicates.
            //
            // Guiding principles for index assignment to leaf predicates:
            //
            // 1. If we assign an index to {x:{$gt: 5}} we should assign the same index to
            //    {x:{$lt: 50}}.  That is, an index assignment should include all predicates
            //    over its leading field.
            //
            // 2. If we have the index {a:1, b:1} and we assign it to {a: 5} we should assign it
            //    to {b:7}, since with a predicate over the first field of the compound index,
            //    the second field can be bounded as well.  We may only assign indices to predicates
            //    if all fields to the left of the index field are constrained.

            // Enumeration of an AND:
            //
            // If there are any mandatory indices, we assign them one at a time.  After we have
            // assigned all of them, we stop assigning indices.
            //
            // Otherwise: We assign each index in predChoice.  When those are exhausted, we have
            // each subtree enumerate its choices one at a time.  When the last subtree has
            // enumerated its last choices, we are done.
            //
            void resetEnumeration() {
                if (mandatory.size() > 0) {
                    state = AndAssignment::MANDATORY;
                }
                else if (predChoices.size() > 0) {
                    state = AndAssignment::PRED_CHOICES;
                }
                else {
                    verify(subnodes.size() > 0);
                    state = AndAssignment::SUBNODES;
                }
                counter = 0;
            }

            EnumerationState state;
            // We're on the counter-th member of state.
            size_t counter;
        };

        /**
         * Associates indices with predicates.
         */
        struct NodeAssignment {
            scoped_ptr<PredicateAssignment> pred;
            scoped_ptr<OrAssignment> orAssignment;
            scoped_ptr<AndAssignment> newAnd;
            string toString() const;
        };

        /**
         * Allocates a NodeAssignment and associates it with the provided 'expr'.
         *
         * The unique MemoID of the new assignment is outputted in '*id'.
         * The out parameter '*slot' points to the newly allocated NodeAssignment.
         */
        void allocateAssignment(MatchExpression* expr, NodeAssignment** slot, MemoID* id);

        void dumpMemo();

        // Used to label nodes in the order in which we visit in a post-order traversal.
        size_t _inOrderCount;

        // Map from expression to its MemoID.
        unordered_map<MatchExpression*, MemoID> _nodeToId;

        // Map from MemoID to its precomputed solution info.
        unordered_map<MemoID, NodeAssignment*> _memo;

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

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

    struct PlanEnumeratorParams {

        // How many choices do we want when computing ixisect solutions in an AND?
        static const size_t kDefaultMaxIntersectPerAnd = 3;

        PlanEnumeratorParams() : intersect(false),
                                 maxIntersectPerAnd(3) { }

        // Do we provide solutions that use more indices than the minimum required to provide
        // an indexed solution?
        bool intersect;

        // Not owned here.
        MatchExpression* root;

        // Not owned here.
        const vector<IndexEntry>* indices;

        // How many intersect plans are we willing to output from an AND?  Given that we pursue an
        // all-pairs approach, we could wind up creating a lot of enumeration possibilities for
        // certain inputs.
        size_t maxIntersectPerAnd;
    };

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
        PlanEnumerator(const PlanEnumeratorParams& params);

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

        struct AndEnumerableState {
            vector<OneIndexAssignment> assignments;
            vector<MemoID> subnodesToIndex;
        };

        struct AndAssignment {
            AndAssignment() : counter(0) { }

            vector<AndEnumerableState> choices;

            // We're on the counter-th member of state.
            size_t counter;
        };

        struct ArrayAssignment {
            ArrayAssignment() : counter(0) { }
            vector<MemoID> subnodes;
            size_t counter;
        };

        /**
         * Associates indices with predicates.
         */
        struct NodeAssignment {
            scoped_ptr<PredicateAssignment> pred;
            scoped_ptr<OrAssignment> orAssignment;
            scoped_ptr<AndAssignment> andAssignment;
            scoped_ptr<ArrayAssignment> arrayAssignment;
            string toString() const;
        };

        /**
         * Allocates a NodeAssignment and associates it with the provided 'expr'.
         *
         * The unique MemoID of the new assignment is outputted in '*id'.
         * The out parameter '*slot' points to the newly allocated NodeAssignment.
         */
        void allocateAssignment(MatchExpression* expr, NodeAssignment** slot, MemoID* id);

        /**
         * Output index intersection assignments inside of an AND node.
         */
        typedef unordered_map<IndexID, vector<MatchExpression*> > IndexToPredMap;

        /**
         * Generate index intersection assignments given the predicate/index structure in idxToFirst
         * and idxToNotFirst (and the sub-trees in 'subnodes').  Outputs the assignments in
         * 'andAssignment'.
         */
        void enumerateAndIntersect(const IndexToPredMap& idxToFirst,
                                   const IndexToPredMap& idxToNotFirst,
                                   const vector<MemoID>& subnodes,
                                   AndAssignment* andAssignment);

        /**
         * Generate one-index-at-once assignments given the predicate/index structure in idxToFirst
         * and idxToNotFirst (and the sub-trees in 'subnodes').  Outputs the assignments into
         * 'andAssignment'.
         */
        void enumerateOneIndex(const IndexToPredMap& idxToFirst,
                               const IndexToPredMap& idxToNotFirst,
                               const vector<MemoID>& subnodes,
                               AndAssignment* andAssignment);

        /**
         * Try to assign predicates in 'tryCompound' to 'thisIndex' as compound assignments.
         * Output the assignments in 'assign'.
         */
        void compound(const vector<MatchExpression*>& tryCompound,
                      const IndexEntry& thisIndex,
                      OneIndexAssignment* assign);

        void dumpMemo();

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

        // Match expression we're planning for.  Not owned by us.
        MatchExpression* _root;

        // Indices we're allowed to enumerate with.  Not owned here.
        const vector<IndexEntry>* _indices;

        // Do we output >1 index per AND (index intersection)?
        bool _ixisect;

        // How many things do we want from each AND?
        size_t _intersectLimit;
    };

} // namespace mongo

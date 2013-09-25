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
        PlanEnumerator(MatchExpression* root, const vector<BSONObj>* indices);
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

        const vector<BSONObj>* _indices;

        //
        // Memoization strategy
        //

        bool isCompound(size_t idx) {
            return (*_indices)[idx].nFields() > 1;
        }

        void checkCompound(MatchExpression* node);

        // Returns true if node uses an index.
        bool prepMemo(MatchExpression* node);
        void tagMemo(size_t id);
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

            string toString() const {
                if (NULL != pred) {
                    stringstream ss;
                    ss << "predicate, first indices: [";
                    for (size_t i = 0; i < pred->first.size(); ++i) {
                        ss << pred->first[i];
                        if (i < pred->first.size() - 1)
                            ss << ", ";
                    }
                    ss << "], notFirst indices: [";
                    for (size_t i = 0; i < pred->notFirst.size(); ++i) {
                        ss << pred->notFirst[i];
                        if (i < pred->notFirst.size() - 1)
                            ss << ", ";
                    }
                    ss << "], pred: " << pred->expr->toString();
                    return ss.str();
                }
                else if (NULL != andSolution) {
                    stringstream ss;
                    ss << "ONE OF: [";
                    for (size_t i = 0; i < andSolution->subnodes.size(); ++i) {
                        const vector<size_t>& sn = andSolution->subnodes[i];
                        ss << "[";
                        for (size_t j = 0; j < sn.size(); ++j) {
                            ss << sn[j];
                            if (j < sn.size() - 1)
                                ss << ", ";
                        }
                        ss << "]";
                        if (i < andSolution->subnodes.size() - 1)
                            ss << ", ";
                    }
                    ss << "]";
                    return ss.str();
                }
                else {
                    verify(NULL != orSolution);
                    stringstream ss;
                    ss << "ALL OF: [";
                    for (size_t i = 0; i < orSolution->subnodes.size(); ++i) {
                        ss << " " << orSolution->subnodes[i];
                    }
                    ss << "]";
                    return ss.str();
                }
            }
        };

        // Memoization

        // Used to label nodes in the order in which we visit in a post-order traversal.
        size_t inOrderCount;

        // Map from node to its order/ID.
        map<MatchExpression*, size_t> nodeToId;

        // Map from order/ID to a memoized solution.
        map<size_t, NodeSolution*> memo;

        // Enumeration

        // ANDs count through clauses, PREDs count through indices.
        // Index is order/ID.
        // Value is whatever counter that node needs.
        map<size_t, size_t> curEnum;

        // return true if hit the end of the subtree rooted at 'node'.
        //
        // implies either that the next node in parent must increment (if internal node), or if
        // root, that enum is done.
        bool nextEnum(MatchExpression* node);

        bool _done;
    };

} // namespace mongo

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

#include "mongo/db/query/plan_enumerator.h"

#include <set>

#include "mongo/db/query/index_tag.h"

namespace mongo {

    PlanEnumerator::PlanEnumerator(MatchExpression* root,
                                   const PredicateMap* pm,
                                   const vector<BSONObj>* indices)
        : _root(root)
        , _pm(*pm)
        , _indices(*indices) {}

    Status PlanEnumerator::init() {
        if (_pm.size() == 0) {
            return Status(ErrorCodes::BadValue, "Cannot enumerate query without predicates map");
        }

        if (_indices.size() == 0) {
            return Status(ErrorCodes::BadValue, "Cannot enumerate indexed plans with no indices");
        }

        //
        // Legacy Strategy initialization
        //

        _done = false;
        cout << "enumerator received root: " << _root->toString() << endl;

        // If we fail to prepare, there's some OR clause or other index-requiring predicate that
        // doesn't have an index.
        if (!prepLegacyStrategy(_root)) {
            _done = true;
        }
        else {
            // We increment this from the beginning and roll forward.
            _assignedCounter.resize(_leavesRequireIndex.size(), 0);
        }

        cout << "prepped enum base tree " << _root->toString() << endl;

        //
        // Final initialization
        //

        return Status::OK();
    }

    bool PlanEnumerator::getNext(MatchExpression** tree) {
        if (_done) { return false; }

        //
        // Legacy Strategy
        //

        // _assignedCounter is the next selection of indies to try.  We increment after
        // each getNext.

        for (size_t i = 0; i < _leavesRequireIndex.size(); ++i) {
            cout << "Leaf requires index: " << _leavesRequireIndex[i]->toString();

            // XXX: this is a slow lookup due to str stuff
            PredicateMap::const_iterator pmit = _pm.find(_leavesRequireIndex[i]->path().toString());
            verify(pmit != _pm.end());
            const PredicateInfo& pi = pmit->second;
            verify(!pi.relevant.empty());

            // XXX: relevant indices should be array
            set<RelevantIndex>::const_iterator it = pi.relevant.begin();
            for (size_t j = 0; j < _assignedCounter[i]; ++j) {
                ++it;
            }

            // it now points to which idx to assign

            // XXX: ignoring compound indices entirely for now.  can only choose a NOT_FIRST index
            // if we chose a FIRST index for a node and-related to us.  We know what nodes are
            // directly AND-related when we create _leavesRequireIndex.  Cache there somehow.
            // use map of MatchExpression -> some # that represents the and, perhaps the parent.
            // Only need for leaves.
            verify(RelevantIndex::FIRST == it->relevance);

            IndexTag* tag = new IndexTag(it->index);
            _leavesRequireIndex[i]->setTag(tag);
        }

        cout << "enum tag iter tree " << _root->toString() << endl;

        // Move to next index.
        size_t carry = 1;
        for (size_t i = 0; i < _assignedCounter.size(); ++i) {
            if (!carry) break;

            _assignedCounter[i] += carry;

            // The max value is the size of the relevant index set
            PredicateMap::const_iterator it = _pm.find(_leavesRequireIndex[i]->path().toString());
            verify(it != _pm.end());
            const PredicateInfo& pi = it->second;

            if (_assignedCounter[i] >= pi.relevant.size()) {
                _assignedCounter[i] = 0;
                carry = 1;
            }
        }

        if (carry > 0) { _done = true; }

        //
        // _root is now tagged with the indices we use, selected by a strategy above
        //

        // tags are cloned w/the tree clone.
        MatchExpression* ret = _root->shallowClone();
        // clear out copy of tags from tree we walk
        _root->resetTag();

        // TODO: Document thoroughly and/or move up into the planner.
        tagForSort(ret);
        sortUsingTags(ret);

        *tree = ret;
        return true;
    }

    //
    // Legacy strategy.  Each OR clause has one index.
    //

    bool PlanEnumerator::hasIndexAvailable(MatchExpression* node) {
        PredicateMap::const_iterator it = _pm.find(node->path().toString());
        if (it == _pm.end()) {
            return false;
        }
        // XXX XXX: Check to see if we have any entries that are FIRST.  Will not work with compound
        // right now.
        return it->second.relevant.size() > 0;
    }

    bool PlanEnumerator::prepLegacyStrategy(MatchExpression* root) {
        if (root->isLeaf() || root->isArray()) {
            if (!hasIndexAvailable(root)) { return false; }
            _leavesRequireIndex.push_back(root);
            return true;
        }
        else {
            verify(root->isLogical());
            if (MatchExpression::OR == root->matchType()) {
                for (size_t i = 0; i < root->numChildren(); ++i) {
                    MatchExpression* child = root->getChild(i);
                    bool willHaveIndex = prepLegacyStrategy(root->getChild(i));
                    if (!willHaveIndex) {
                        warning() << "OR child " << child->toString() << " won't have idx";
                        return false;
                    }
                }
                // Each of our children has an index so we'll have an index.
                return true;
            }
            else if (MatchExpression::AND == root->matchType()) {
                MatchExpression* expressionToIndex = NULL;
                bool indexAssignedToAtLeastOneChild = false;

                // XXX: is this non-deterministic depending on the order of clauses of the and?  I
                // think it is.  if we see an OR clause first as a child, those have an index
                // assigned, and we hang any further preds off as a filter.  if we see a filter
                // first, we create an ixscan and AND it with any subsequent OR children.
                //
                // A solution here is to sort the children in the desired order, so the ORs are
                // first, if we're really trying to minimize indices.

                for (size_t i = 0; i < root->numChildren(); ++i) {
                    MatchExpression* child = root->getChild(i);

                    // If the child requires an index, use an index.
                    // TODO: Text goes here.
                    if (MatchExpression::GEO_NEAR == child->matchType()) {
                        // We must use an index in this case.
                        if (!prepLegacyStrategy(child)) {
                            return false;
                        }
                        indexAssignedToAtLeastOneChild = true;
                    }
                    else if (child->isLogical()) {
                        // We must use an index in this case as well.

                        // We've squashed AND-AND and OR-OR into AND/OR respectively, so this should
                        // be an OR.
                        verify(MatchExpression::OR == child->matchType());
                        if (!prepLegacyStrategy(child)) {
                            return false;
                        }
                        indexAssignedToAtLeastOneChild = true;
                    }
                    else {
                        verify(child->isArray() || child->isLeaf());

                        if (!indexAssignedToAtLeastOneChild && hasIndexAvailable(child)) {
                            verify(NULL == expressionToIndex);
                            expressionToIndex = child;
                            indexAssignedToAtLeastOneChild = true;
                        }
                    }
                }

                // We've only filled out expressionToIndex if it has an index available.
                if (NULL != expressionToIndex) {
                    verify(prepLegacyStrategy(expressionToIndex));
                }

                return indexAssignedToAtLeastOneChild;
            }
            else {
                warning() << "Can't deal w/logical node in enum: " << root->toString();
                verify(0);
                return false;
            }
        }
    }

} // namespace mongo

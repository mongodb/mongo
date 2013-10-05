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

#include "mongo/db/query/indexability.h"
#include "mongo/db/query/index_tag.h"

namespace mongo {

    PlanEnumerator::PlanEnumerator(MatchExpression* root, const vector<IndexEntry>* indices)
        : _root(root), _indices(indices) { }

    PlanEnumerator::~PlanEnumerator() {
        for (unordered_map<NodeID, NodeAssignment*>::iterator it = _memo.begin(); it != _memo.end(); ++it) {
            delete it->second;
        }
    }

    Status PlanEnumerator::init() {
        _inOrderCount = 0;
        _done = false;

        cout << "enumerator received root: " << _root->toString() << endl;

        // Fill out our memo structure from the tagged _root.
        _done = !prepMemo(_root);

        // Dump the tags.  We replace them with IndexTag instances.
        _root->resetTag();

        // cout << "root post-memo: " << _root->toString() << endl;

        cout << "memo dump:\n";
        verify(_inOrderCount == _memo.size());
        for (size_t i = 0; i < _inOrderCount; ++i) {
            cout << "Node #" << i << ": " << _memo[i]->toString() << endl;
        }

        if (!_done) {
            // Tag with our first solution.
            tagMemo(_nodeToId[_root]);
        }

        return Status::OK();
    }

    bool PlanEnumerator::isCompound(IndexID idx) {
        return (*_indices)[idx].keyPattern.nFields() > 1;
    }

    string PlanEnumerator::NodeAssignment::toString() const {
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
        else if (NULL != newAnd) {
            stringstream ss;
            ss << "ONE OF: [";
            for (size_t i = 0; i < newAnd->subnodes.size(); ++i) {
                ss << "[" << newAnd->subnodes[i] << "]";
                if (i < newAnd->subnodes.size() - 1) {
                    ss << ", ";
                }
            }
            for (size_t i = 0; i < newAnd->predChoices.size(); ++i) {
                const OneIndexAssignment& oie = newAnd->predChoices[i];
                ss << "IDX#" << oie.index << " for preds: ";
                for (size_t j = 0; j < oie.preds.size(); ++j) {
                    ss << oie.preds[j]->toString() << ", ";
                }
            }
            return ss.str();
        }
        else {
            verify(NULL != orAssignment);
            stringstream ss;
            ss << "ALL OF: [";
            for (size_t i = 0; i < orAssignment->subnodes.size(); ++i) {
                ss << " " << orAssignment->subnodes[i];
            }
            ss << "]";
            return ss.str();
        }
    }

    bool PlanEnumerator::getNext(MatchExpression** tree) {
        if (_done) { return false; }
        *tree = _root->shallowClone();

        // Adds tags to internal nodes indicating whether or not they are indexed.
        tagForSort(*tree);

        // Sorts nodes by tags, grouping similar tags together.
        sortUsingTags(*tree);

        _root->resetTag();
        // TODO: enumerate >1 plan
        _done = true;
        return true;
    }

    bool PlanEnumerator::prepMemo(MatchExpression* node) {
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            // TODO: This is done for everything, maybe have NodeAssignment* newMemo(node)?
            size_t myID = _inOrderCount++;
            _nodeToId[node] = myID;
            NodeAssignment* soln = new NodeAssignment();
            _memo[_nodeToId[node]] = soln;

            _curEnum[myID] = 0;

            // Fill out the NodeAssignment.
            soln->pred.reset(new PredicateAssignment());
            if (NULL != node->getTag()) {
                RelevantTag* rt = static_cast<RelevantTag*>(node->getTag());
                soln->pred->first.swap(rt->first);
                soln->pred->notFirst.swap(rt->notFirst);
            }
            soln->pred->expr = node;
            // There's no guarantee that we can use any of the notFirst indices, so we only claim to
            // be indexed when there are 'first' indices.
            return soln->pred->first.size() > 0;
        }
        else if (MatchExpression::OR == node->matchType()) {
            // For an OR to be indexed all its children must be indexed.
            bool indexed = true;
            for (size_t i = 0; i < node->numChildren(); ++i) {
                if (!prepMemo(node->getChild(i))) {
                    indexed = false;
                }
            }

            size_t myID = _inOrderCount++;
            _nodeToId[node] = myID;
            NodeAssignment* soln = new NodeAssignment();
            _memo[_nodeToId[node]] = soln;

            OrAssignment* orAssignment = new OrAssignment();
            for (size_t i = 0; i < node->numChildren(); ++i) {
                orAssignment->subnodes.push_back(_nodeToId[node->getChild(i)]);
            }
            soln->orAssignment.reset(orAssignment);
            return indexed;
        }
        else if (MatchExpression::AND == node->matchType() || Indexability::arrayUsesIndexOnChildren(node)) {
            // map from idx id to children that have a pred over it.
            unordered_map<IndexID, vector<MatchExpression*> > idxToFirst;
            unordered_map<IndexID, vector<MatchExpression*> > idxToNotFirst;

            NewAndAssignment* newAndAssignment = new NewAndAssignment();
            for (size_t i = 0; i < node->numChildren(); ++i) {
                MatchExpression* child = node->getChild(i);

                if (Indexability::nodeCanUseIndexOnOwnField(child)) {
                    RelevantTag* rt = static_cast<RelevantTag*>(child->getTag());
                    for (size_t j = 0; j < rt->first.size(); ++j) {
                        idxToFirst[rt->first[j]].push_back(child);
                    }
                    for (size_t j = 0 ; j< rt->notFirst.size(); ++j) {
                        idxToNotFirst[rt->notFirst[j]].push_back(child);
                    }
                }
                else {
                    if (prepMemo(child)) {
                        size_t childID = _nodeToId[child];
                        newAndAssignment->subnodes.push_back(childID);
                    }
                }
            }

            // At this point we know how many indices the AND's predicate children are over.
            newAndAssignment->predChoices.resize(idxToFirst.size());

            // This iterates through the predChoices.
            size_t predChoicesIdx = 0;

            // For each FIRST, we assign nodes to it.
            for (unordered_map<IndexID, vector<MatchExpression*> >::iterator it = idxToFirst.begin(); it != idxToFirst.end(); ++it) {
                OneIndexAssignment* assign = &newAndAssignment->predChoices[predChoicesIdx];
                ++predChoicesIdx;

                // Fill out the OneIndexAssignment with the preds that are over the first field.
                assign->index = it->first;
                // We can swap because we're never touching idxToFirst again after this loop over it.
                assign->preds.swap(it->second);
                assign->positions.resize(assign->preds.size(), 0);

                //
                // Compound analysis here and below.
                //

                // Don't compound on multikey indices. (XXX: not whole story...)
                if ((*_indices)[it->first].multikey) { continue; }

                // Grab the expressions that are notFirst for the index whose assignments we're filling out.
                unordered_map<size_t, vector<MatchExpression*> >::const_iterator compoundIt = idxToNotFirst.find(it->first);
                if (compoundIt == idxToNotFirst.end()) { continue; }
                const vector<MatchExpression*>& tryCompound = compoundIt->second;

                // Walk over the key pattern trying to find 
                BSONObjIterator kpIt((*_indices)[it->first].keyPattern);
                // Skip the first elt as it's already assigned.
                kpIt.next();
                size_t posInIdx = 0;
                while (kpIt.more()) {
                    BSONElement keyElt = kpIt.next();
                    ++posInIdx;
                    bool fieldAssigned = false;
                    for (size_t j = 0; j < tryCompound.size(); ++j) {
                        MatchExpression* maybe = tryCompound[j];
                        // Sigh we grab the full path from the relevant tag.
                        RelevantTag* rt = static_cast<RelevantTag*>(maybe->getTag());
                        if (keyElt.fieldName() == rt->path) {
                            assign->preds.push_back(maybe);
                            assign->positions.push_back(posInIdx);
                            fieldAssigned = true;
                        }
                    }
                    // If we have (a,b,c) and we can't assign something to 'b' don't try
                    // to assign something to 'c'.
                    if (!fieldAssigned) { break; }
                }
            }

            // Enumeration detail: We start enumerating with the first predChoice.  To make sure that
            // we always output the geoNear index, put it first.
            //
            // TODO: We can compute this more "on the fly" but it's clearer to see what's going on when
            // we do this as a separate step.
            for (size_t i = 0; i < newAndAssignment->predChoices.size(); ++i) {
                const OneIndexAssignment& oie = newAndAssignment->predChoices[i];
                bool foundGeoNear = false;

                for (size_t j = 0; j < oie.preds.size(); ++j) {
                    MatchExpression* expr = oie.preds[j];
                    if (MatchExpression::GEO_NEAR == expr->matchType()) {
                        // If the GEO_NEAR idx is already at position 0 we'll choose it.
                        // TODO: We have to be smarter when we enumerate >1 plan.
                        if (0 != i) {
                            // Move the GEO_NEAR supplying index to the first choice spot.
                            std::swap(newAndAssignment->predChoices[0], newAndAssignment->predChoices[i]);
                        }
                        foundGeoNear = true;
                        break;
                    }
                }
                if (foundGeoNear) { break; }
            }

            // Normal node allocation stuff.
            size_t myID = _inOrderCount++;
            _nodeToId[node] = myID;
            NodeAssignment* soln = new NodeAssignment();
            _memo[_nodeToId[node]] = soln;
            _curEnum[myID] = 0;

            // Takes ownership.
            soln->newAnd.reset(newAndAssignment);
            return newAndAssignment->subnodes.size() > 0 || newAndAssignment->predChoices.size() > 0;
        }

        // Don't know what the node is at this point.
        return false;
    }

    void PlanEnumerator::tagMemo(size_t id) {
        NodeAssignment* soln = _memo[id];
        verify(NULL != soln);

        if (NULL != soln->pred) {
            verify(NULL == soln->pred->expr->getTag());
            // There may be no indices assignable.  That's OK.
            if (0 != soln->pred->first.size()) {
                // We only assign indices that can be used without any other predicate.
                // Compound is dealt with in the AND processing; there must be an AND to use
                // a notFirst index..
                verify(_curEnum[id] < soln->pred->first.size());
                soln->pred->expr->setTag(new IndexTag(soln->pred->first[_curEnum[id]]));
            }
        }
        else if (NULL != soln->orAssignment) {
            for (size_t i = 0; i < soln->orAssignment->subnodes.size(); ++i) {
                tagMemo(soln->orAssignment->subnodes[i]);
            }
            // TODO: Who checks to make sure that we tag all nodes of an OR?  We should
            // know this early.
        }
        else if (NULL != soln->newAnd) {
            size_t curEnum = _curEnum[id];
            if (curEnum < soln->newAnd->predChoices.size()) {
                const OneIndexAssignment assign = soln->newAnd->predChoices[curEnum];
                for (size_t i = 0; i < assign.preds.size(); ++i) {
                    MatchExpression* pred = assign.preds[i];
                    verify(NULL == pred->getTag());
                    pred->setTag(new IndexTag(assign.index, assign.positions[i]));
                }
            }
            else {
                curEnum -= soln->newAnd->predChoices.size();
                if (curEnum < soln->newAnd->subnodes.size()) {
                    tagMemo(soln->newAnd->subnodes[curEnum]);
                }
            }
        }
        else {
            verify(0);
        }
    }

    bool PlanEnumerator::nextMemo(size_t id) {
        return false;
    }

} // namespace mongo

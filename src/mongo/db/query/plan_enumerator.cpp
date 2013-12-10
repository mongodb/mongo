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

#include "mongo/db/query/plan_enumerator.h"

#include <set>

#include "mongo/db/query/indexability.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/qlog.h"

namespace mongo {

    PlanEnumerator::PlanEnumerator(MatchExpression* root, const vector<IndexEntry>* indices)
        : _root(root), _indices(indices) { }

    PlanEnumerator::~PlanEnumerator() {
        typedef unordered_map<MemoID, NodeAssignment*> MemoMap;
        for (MemoMap::iterator it = _memo.begin(); it != _memo.end(); ++it) {
            delete it->second;
        }
    }

    Status PlanEnumerator::init() {
        _inOrderCount = 0;
        _done = false;

        QLOG() << "enumerator received root:\n" << _root->toString() << endl;

        // Fill out our memo structure from the tagged _root.
        _done = !prepMemo(_root);

        // Dump the tags.  We replace them with IndexTag instances.
        _root->resetTag();

        return Status::OK();
    }

    void PlanEnumerator::dumpMemo() {
        verify(_inOrderCount == _memo.size());
        for (size_t i = 0; i < _inOrderCount; ++i) {
            QLOG() << "[Node #" << i << "]: " << _memo[i]->toString() << endl;
        }
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
            ss << "], pred: " << pred->expr->toString();
            ss << " indexToAssign: " << pred->indexToAssign;
            return ss.str();
        }
        else if (NULL != newAnd) {
            stringstream ss;
            ss << "AND enumstate: ";
            if (AndAssignment::MANDATORY == newAnd->state) {
                ss << "mandatory";
            }
            else if (AndAssignment::PRED_CHOICES == newAnd->state) {
                ss << "pred_choices";
            }
            else {
                verify(AndAssignment::SUBNODES == newAnd->state);
                ss << "subnodes";
            }
            ss << " counter " << newAnd->counter;
            ss << "\nsubnodes: [";
            for (size_t i = 0; i < newAnd->subnodes.size(); ++i) {
                ss << newAnd->subnodes[i];
                if (i < newAnd->subnodes.size() - 1) {
                    ss << " , ";
                }
            }
            ss << "]\n";
            for (size_t i = 0; i < newAnd->predChoices.size(); ++i) {
                const OneIndexAssignment& oie = newAnd->predChoices[i];
                ss << "idx " << oie.index << " for preds:\n";
                for (size_t j = 0; j < oie.preds.size(); ++j) {
                    ss << "\t" << oie.preds[j]->toString();
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

        // Tag with our first solution.
        tagMemo(_nodeToId[_root]);

        *tree = _root->shallowClone();
        tagForSort(*tree);
        sortUsingTags(*tree);

        _root->resetTag();
        QLOG() << "Enumerator: memo right before moving:\n";
        dumpMemo();
        _done = nextMemo(_nodeToId[_root]);
        QLOG() << "Enumerator: memo right after moving:\n";
        dumpMemo();
        return true;
    }

    void PlanEnumerator::allocateAssignment(MatchExpression* expr, NodeAssignment** assign,
                                            MemoID* id) {
        size_t newID = _inOrderCount++;
        verify(_nodeToId.end() == _nodeToId.find(expr));
        _nodeToId[expr] = newID;
        verify(_memo.end() == _memo.find(newID));
        NodeAssignment* newAssignment = new NodeAssignment();
        _memo[newID] = newAssignment;
        *assign = newAssignment;
        *id = newID;
    }

    bool PlanEnumerator::prepMemo(MatchExpression* node) {
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            // We only get here if our parent is an OR, an array operator, or we're the root.

            // If we have no index tag there are no indices we can use.
            if (NULL == node->getTag()) { return false; }

            RelevantTag* rt = static_cast<RelevantTag*>(node->getTag());
            // In order to definitely use an index it must be prefixed with our field.
            // We don't consider notFirst indices here because we must be AND-related to a node
            // that uses the first spot in that index, and we currently do not know that
            // unless we're in an AND node.
            if (0 == rt->first.size()) { return false; }

            // We know we can use an index, so grab a memo spot.
            size_t myMemoID;
            NodeAssignment* assign;
            allocateAssignment(node, &assign, &myMemoID);

            assign->pred.reset(new PredicateAssignment());
            assign->pred->expr = node;
            assign->pred->first.swap(rt->first);
            return true;
        }
        else if (MatchExpression::OR == node->matchType()) {
            // For an OR to be indexed, all its children must be indexed.
            for (size_t i = 0; i < node->numChildren(); ++i) {
                if (!prepMemo(node->getChild(i))) {
                    return false;
                }
            }

            // If we're here we're fully indexed and can be in the memo.
            size_t myMemoID;
            NodeAssignment* assign;
            allocateAssignment(node, &assign, &myMemoID);

            OrAssignment* orAssignment = new OrAssignment();
            for (size_t i = 0; i < node->numChildren(); ++i) {
                orAssignment->subnodes.push_back(_nodeToId[node->getChild(i)]);
            }
            assign->orAssignment.reset(orAssignment);
            return true;
        }
        else if (MatchExpression::AND == node->matchType() || Indexability::arrayUsesIndexOnChildren(node)) {
            // map from idx id to children that have a pred over it.
            unordered_map<IndexID, vector<MatchExpression*> > idxToFirst;
            unordered_map<IndexID, vector<MatchExpression*> > idxToNotFirst;

            vector<MemoID> subnodes;

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
                        verify(_nodeToId.end() != _nodeToId.find(child));
                        size_t childID = _nodeToId[child];
                        subnodes.push_back(childID);
                    }
                }
            }

            if (idxToFirst.empty() && (subnodes.size() == 0)) { return false; }

            AndAssignment* newAndAssignment = new AndAssignment();
            newAndAssignment->subnodes.swap(subnodes);

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
                // If it's a multikey index, we can't intersect the bounds, so we only want one pred.
                if ((*_indices)[it->first].multikey) {
                    // XXX: pick a better pred than the first one that happens to wander in.
                    // XXX: see and3.js, indexq.js, arrayfind7.js
                    QLOG() << "Index " << (*_indices)[it->first].keyPattern.toString()
                         << " is multikey but has >1 pred possible, should be smarter"
                         << " here and pick the best one"
                         << endl;
                    assign->preds.resize(1);
                }
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

            // Some predicates *require* an index.  We stuff these in 'mandatory' inside of the
            // AndAssignment.
            //
            // TODO: We can compute this "on the fly" above somehow, but it's clearer to see what's
            // going on when we do this as a separate step.
            //
            // TODO: Consider annotating mandatory indices in the planner as part of the available
            // index tagging.

            // Note we're not incrementing 'i' in the loop.  We may erase the i-th element.
            for (size_t i = 0; i < newAndAssignment->predChoices.size();) {
                const OneIndexAssignment& oie = newAndAssignment->predChoices[i];
                bool hasPredThatRequiresIndex = false;

                for (size_t j = 0; j < oie.preds.size(); ++j) {
                    MatchExpression* expr = oie.preds[j];
                    if (MatchExpression::GEO_NEAR == expr->matchType()) {
                        hasPredThatRequiresIndex = true;
                        break;
                    }
                    if (MatchExpression::TEXT == expr->matchType()) {
                        hasPredThatRequiresIndex = true;
                        break;
                    }
                }

                if (hasPredThatRequiresIndex) {
                    newAndAssignment->mandatory.push_back(oie);
                    newAndAssignment->predChoices.erase(newAndAssignment->predChoices.begin() + i);
                }
                else {
                    ++i;
                }
            }

            newAndAssignment->resetEnumeration();

            size_t myMemoID;
            NodeAssignment* assign;
            allocateAssignment(node, &assign, &myMemoID);
            // Takes ownership.
            assign->newAnd.reset(newAndAssignment);
            return true;
        }

        // Don't know what the node is at this point.
        return false;
    }

    void PlanEnumerator::tagMemo(size_t id) {
        QLOG() << "Tagging memoID " << id << endl;
        NodeAssignment* assign = _memo[id];
        verify(NULL != assign);

        if (NULL != assign->pred) {
            PredicateAssignment* pa = assign->pred.get();
            verify(NULL == pa->expr->getTag());
            verify(pa->indexToAssign < pa->first.size());
            pa->expr->setTag(new IndexTag(pa->first[pa->indexToAssign]));
        }
        else if (NULL != assign->orAssignment) {
            OrAssignment* oa = assign->orAssignment.get();
            for (size_t i = 0; i < oa->subnodes.size(); ++i) {
                tagMemo(oa->subnodes[i]);
            }
        }
        else if (NULL != assign->newAnd) {
            AndAssignment* aa = assign->newAnd.get();

            if (AndAssignment::MANDATORY == aa->state) {
                verify(aa->counter < aa->mandatory.size());
                const OneIndexAssignment& assign = aa->mandatory[aa->counter];
                for (size_t i = 0; i < assign.preds.size(); ++i) {
                    MatchExpression* pred = assign.preds[i];
                    verify(NULL == pred->getTag());
                    pred->setTag(new IndexTag(assign.index, assign.positions[i]));
                }
            }
            else if (AndAssignment::PRED_CHOICES == aa->state) {
                verify(aa->counter < aa->predChoices.size());
                const OneIndexAssignment& assign = aa->predChoices[aa->counter];
                for (size_t i = 0; i < assign.preds.size(); ++i) {
                    MatchExpression* pred = assign.preds[i];
                    verify(NULL == pred->getTag());
                    pred->setTag(new IndexTag(assign.index, assign.positions[i]));
                }
            }
            else {
                verify(AndAssignment::SUBNODES == aa->state);
                verify(aa->counter < aa->subnodes.size());
                tagMemo(aa->subnodes[aa->counter]);
            }
        }
        else {
            verify(0);
        }
    }

    bool PlanEnumerator::nextMemo(size_t id) {
        NodeAssignment* assign = _memo[id];
        verify(NULL != assign);

        if (NULL != assign->pred) {
            PredicateAssignment* pa = assign->pred.get();
            pa->indexToAssign++;
            if (pa->indexToAssign >= pa->first.size()) {
                pa->indexToAssign = 0;
                return true;
            }
            return false;
        }
        else if (NULL != assign->orAssignment) {
            // OR doesn't have any enumeration state.  It just walks through telling its children to
            // move forward.
            OrAssignment* oa = assign->orAssignment.get();
            for (size_t i = 0; i < oa->subnodes.size(); ++i) {
                // If there's no carry, we just stop.  If there's a carry, we move the next child
                // forward.
                if (!nextMemo(oa->subnodes[i])) {
                    return false;
                }
            }
            // If we're here, the last subnode had a carry, therefore the OR has a carry.
            return true;
        }
        else if (NULL != assign->newAnd) {
            AndAssignment* aa = assign->newAnd.get();

            // If we're still walking through the mandatory assignments.
            if (AndAssignment::MANDATORY == aa->state) {
                verify(aa->mandatory.size() > 0);
                ++aa->counter;

                // Is there a subsequent MANDATORY assignment to make?
                if (aa->counter < aa->mandatory.size()) {
                    // If so, there is no carry.
                    return false;
                }

                // If we have any mandatory indices and we're trying to move on, stop and report a
                // carry.  We only enumerate over mandatory indices if we have any.
                aa->resetEnumeration();
                return true;
            }

            if (AndAssignment::PRED_CHOICES == aa->state) {
                ++aa->counter;

                // Still have a predChoice to output.
                if (aa->counter < aa->predChoices.size()) {
                    return false;
                }

                // We (may) move to outputting PRED_CHOICES.
                if (0 == aa->subnodes.size()) {
                    aa->resetEnumeration();
                    return true;
                }
                else {
                    // Next output comes from the 0-th subnode.
                    aa->counter = 0;
                    aa->state = AndAssignment::SUBNODES;
                    return false;
                }
            }

            verify(AndAssignment::SUBNODES == aa->state);
            verify(aa->subnodes.size() > 0);
            verify(aa->counter < aa->subnodes.size());

            // Tell the subtree to move to its next state.
            if (nextMemo(aa->subnodes[aa->counter])) {
                // If the memo is done, move on to the next subnode.
                aa->counter++;
            }
            else {
                // Otherwise, can keep on enumerating through this subtree, as it's not done
                // enumerating its states.
                return false;
            }

            if (aa->counter >= aa->subnodes.size()) {
                // Start from the beginning.  We got a carry from our last subnode.
                aa->resetEnumeration();
                return true;
            }
        }
        else {
            verify(0);
        }

        return false;
    }

} // namespace mongo

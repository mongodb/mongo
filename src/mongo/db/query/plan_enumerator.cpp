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

    PlanEnumerator::PlanEnumerator(const PlanEnumeratorParams& params)
        : _root(params.root),
          _indices(params.indices),
          _ixisect(params.intersect) { }

    PlanEnumerator::~PlanEnumerator() {
        typedef unordered_map<MemoID, NodeAssignment*> MemoMap;
        for (MemoMap::iterator it = _memo.begin(); it != _memo.end(); ++it) {
            delete it->second;
        }
    }

    Status PlanEnumerator::init() {
        QLOG() << "enumerator received root:\n" << _root->toString() << endl;

        // Fill out our memo structure from the tagged _root.
        _done = !prepMemo(_root);

        // Dump the tags.  We replace them with IndexTag instances.
        _root->resetTag();

        return Status::OK();
    }

    void PlanEnumerator::dumpMemo() {
        for (size_t i = 0; i < _memo.size(); ++i) {
            QLOG() << "[Node #" << i << "]: " << _memo[i]->toString() << endl;
        }
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
        else if (NULL != andAssignment) {
            stringstream ss;
            ss << "AND enumstate counter " << andAssignment->counter;
            for (size_t i = 0; i < andAssignment->choices.size(); ++i) {
                ss << "\nchoice " << i << ":\n";
                const AndEnumerableState& state = andAssignment->choices[i];
                ss << "\tsubnodes: ";
                for (size_t j = 0; j < state.subnodesToIndex.size(); ++j) {
                    ss << state.subnodesToIndex[j] << " ";
                }
                ss << endl;
                for (size_t j = 0; j < state.assignments.size(); ++j) {
                    const OneIndexAssignment& oie = state.assignments[j];
                    ss << "\tidx[" << oie.index << "]\n";

                    for (size_t k = 0; k < oie.preds.size(); ++k) {
                        ss << "\t\tpos " << oie.positions[k]
                           << " pred " << oie.preds[k]->toString() << endl;
                    }
                }
            }
            return ss.str();
        }
        else if (NULL != arrayAssignment) {
            stringstream ss;
            ss << "ARRAY SUBNODES enumstate " << arrayAssignment->counter << "/ ONE OF: [";
            for (size_t i = 0; i < arrayAssignment->subnodes.size(); ++i) {
                ss << " " << arrayAssignment->subnodes[i];
            }
            ss << "]";
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
        size_t newID = _memo.size();
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
        else if (Indexability::arrayUsesIndexOnChildren(node)) {
            // Add each of our children as a subnode.  We enumerate through each subnode one at a
            // time until it's exhausted then we move on.
            auto_ptr<ArrayAssignment> aa(new ArrayAssignment());

            // For an OR to be indexed, all its children must be indexed.
            for (size_t i = 0; i < node->numChildren(); ++i) {
                if (prepMemo(node->getChild(i))) {
                    aa->subnodes.push_back(_nodeToId[node->getChild(i)]);
                }
            }

            if (0 == aa->subnodes.size()) { return false; }

            size_t myMemoID;
            NodeAssignment* assign;
            allocateAssignment(node, &assign, &myMemoID);

            assign->arrayAssignment.reset(aa.release());
            return true;
        }
        else if (MatchExpression::AND == node->matchType()) {
            typedef unordered_map<IndexID, vector<MatchExpression*> > IndexToPredMap;
            // map from idx id to children that have a pred over it.
            IndexToPredMap idxToFirst;
            IndexToPredMap idxToNotFirst;

            // Children that aren't predicates.
            vector<MemoID> subnodes;

            // Indices we *must* use.  TEXT or GEO.
            set<IndexID> mandatoryIndices;

            // Go through our children and see if they're preds or logical subtrees.
            for (size_t i = 0; i < node->numChildren(); ++i) {
                MatchExpression* child = node->getChild(i);

                if (Indexability::nodeCanUseIndexOnOwnField(child)) {
                    bool childRequiresIndex = (child->matchType() == MatchExpression::GEO_NEAR
                                            || child->matchType() == MatchExpression::TEXT);

                    RelevantTag* rt = static_cast<RelevantTag*>(child->getTag());

                    for (size_t j = 0; j < rt->first.size(); ++j) {
                        idxToFirst[rt->first[j]].push_back(child);
                        if (childRequiresIndex) {
                            // We could have >1 index that could be used to answer the pred for
                            // things like geoNear.  Just pick the first and make it mandatory.
                            mandatoryIndices.insert(rt->first[j]);
                        }
                    }

                    for (size_t j = 0 ; j< rt->notFirst.size(); ++j) {
                        idxToNotFirst[rt->notFirst[j]].push_back(child);
                        if (childRequiresIndex) {
                            // See comment above about mandatory indices.
                            mandatoryIndices.insert(rt->notFirst[j]);
                        }
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

            // If none of our children can use indices, bail out.
            if (idxToFirst.empty() && (subnodes.size() == 0)) { return false; }

            // At least one child can use an index, so we can create a memo entry.
            AndAssignment* andAssignment = new AndAssignment();

            size_t myMemoID;
            NodeAssignment* nodeAssignment;
            allocateAssignment(node, &nodeAssignment, &myMemoID);
            // Takes ownership.
            nodeAssignment->andAssignment.reset(andAssignment);

            // Only near queries and text queries have mandatory indices.
            // In this case there's no point to enumerating anything here; both geoNear
            // and text do fetches internally so we can't use any other indices in conjunction.
            if (mandatoryIndices.size() > 0) {
                // Just use the first mandatory index, why not.
                IndexToPredMap::iterator it = idxToFirst.find(*mandatoryIndices.begin());

                OneIndexAssignment indexAssign;

                // This is the index we assign to.
                indexAssign.index = it->first;

                const IndexEntry& thisIndex = (*_indices)[it->first];

                // If the index is multikey, we only assign one pred to it.  We also skip
                // compounding.  TODO: is this also true for 2d and 2dsphere indices?  can they be
                // multikey but still compoundable?  (How do we get covering for them?)
                if (thisIndex.multikey) {
                    indexAssign.preds.push_back(it->second[0]);
                    indexAssign.positions.push_back(0);
                }
                else {
                    // The index isn't multikey.  Assign all preds to it.  The planner will
                    // do something smart with the bounds.
                    indexAssign.preds = it->second;

                    // Since everything in assign.preds prefixes the index, they all go
                    // at position '0' in the index, the first position.
                    indexAssign.positions.resize(indexAssign.preds.size(), 0);

                    // And now we begin compound analysis.

                    // Find everything that could use assign.index but isn't a pred over
                    // the first field of that index.
                    IndexToPredMap::iterator compIt = idxToNotFirst.find(indexAssign.index);
                    if (compIt != idxToNotFirst.end()) {
                        compound(compIt->second, thisIndex, &indexAssign);
                    }
                }

                AndEnumerableState state;
                state.assignments.push_back(indexAssign);
                andAssignment->choices.push_back(state);
                return true;
            }

            if (_ixisect) {
                // Hardcoded "look at all power sets of size 2" search.
                for (IndexToPredMap::iterator firstIt = idxToFirst.begin();
                     firstIt != idxToFirst.end(); ++firstIt) {

                    const IndexEntry& firstIndex = (*_indices)[firstIt->first];

                    // Create the assignment for firstIt.
                    OneIndexAssignment firstAssign;
                    firstAssign.index = firstIt->first;
                    firstAssign.preds = firstIt->second;
                    // Since everything in assign.preds prefixes the index, they all go
                    // at position '0' in the index, the first position.
                    firstAssign.positions.resize(firstAssign.preds.size(), 0);

                    // We create a scan per predicate so if we have >1 predicate we'll already
                    // have at least 2 scans (one predicate per scan as the planner can't
                    // intersect bounds when the index is multikey), so we stop here.
                    if (firstIndex.multikey && firstAssign.preds.size() > 1) {
                        AndEnumerableState state;
                        state.assignments.push_back(firstAssign);
                        andAssignment->choices.push_back(state);
                        continue;
                    }

                    // Output (subnode, firstAssign) pairs.
                    for (size_t i = 0; i < subnodes.size(); ++i) {
                        AndEnumerableState indexAndSubnode;
                        indexAndSubnode.assignments.push_back(firstAssign);
                        indexAndSubnode.subnodesToIndex.push_back(subnodes[i]);
                        andAssignment->choices.push_back(indexAndSubnode);
                    }

                    // We keep track of what preds were compounded and we DO NOT let them become
                    // additional index assignments.  Example: what if firstAssign is the index (x,
                    // y) and we're considering index y?  We don't want to assign 'y' to anything we
                    // compounded with.
                    set<MatchExpression*> predsAssigned;

                    // We compound on firstAssign, if applicable.
                    IndexToPredMap::iterator firstIndexCompound =
                        idxToNotFirst.find(firstAssign.index);

                    // Can't compound with multikey indices.
                    if (!firstIndex.multikey && firstIndexCompound != idxToNotFirst.end()) {
                        // Assigns MatchExpressions to compound idx.
                        compound(firstIndexCompound->second, firstIndex, &firstAssign);
                    }

                    // Exclude all predicates in firstAssign from future assignments.
                    for (size_t i = 0; i < firstAssign.preds.size(); ++i) {
                        predsAssigned.insert(firstAssign.preds[i]);
                    }

                    // Start looking at all other indices to find one that we want to bundle
                    // with firstAssign.
                    IndexToPredMap::iterator secondIt = firstIt;
                    secondIt++;
                    for (; secondIt != idxToFirst.end(); secondIt++) {
                        const IndexEntry& secondIndex = (*_indices)[secondIt->first];

                        // If the other index we're considering is multikey with >1 pred, we don't
                        // want to have it as an additional assignment.  Eventually, firstIt will be
                        // equal to the current value of secondIt and we'll assign every pred for
                        // this mapping to the index.
                        if (secondIndex.multikey && secondIt->second.size() > 1) {
                            continue;
                        }

                        OneIndexAssignment secondAssign;
                        secondAssign.index = secondIt->first;
                        const vector<MatchExpression*>& preds = secondIt->second;
                        for (size_t i = 0; i < preds.size(); ++i) {
                            if (predsAssigned.end() == predsAssigned.find(preds[i])) {
                                secondAssign.preds.push_back(preds[i]);
                                secondAssign.positions.push_back(0);
                            }
                        }

                        // Every predicate that would use this index is already assigned in
                        // firstAssign.
                        if (0 == secondAssign.preds.size()) { continue; }

                        IndexToPredMap::iterator secondIndexCompound =
                            idxToNotFirst.find(secondAssign.index);

                        if (!secondIndex.multikey && secondIndexCompound != idxToNotFirst.end()) {
                            // We must remove any elements of 'predsAssigned' from consideration.
                            vector<MatchExpression*> tryCompound;
                            const vector<MatchExpression*>& couldCompound
                                = secondIndexCompound->second;
                            for (size_t i = 0; i < couldCompound.size(); ++i) {
                                if (predsAssigned.end() == predsAssigned.find(couldCompound[i])) {
                                    tryCompound.push_back(couldCompound[i]);
                                }
                            }
                            if (tryCompound.size()) {
                                compound(tryCompound, secondIndex, &secondAssign);
                            }
                        }

                        AndEnumerableState state;
                        state.assignments.push_back(firstAssign);
                        state.assignments.push_back(secondAssign);
                        andAssignment->choices.push_back(state);
                    }
                }

                // XXX: Do we just want one subnode at a time?  We can use far more than 2 indices
                // at once doing this very easily.  If we want to restrict the # of indices the
                // children use, when we memoize the subtree above we can restrict it to 1 index at
                // a time.  This can get tricky if we want both an intersection and a 1-index memo
                // entry, since our state change is simple and we don't traverse the memo in any
                // targeted way.  Should also verify that having a one-to-many mapping of
                // MatchExpression to MemoID doesn't break anything.  This approach errors on the
                // side of "too much indexing."
                for (size_t i = 0; i < subnodes.size(); ++i) {
                    for (size_t j = i + 1; j < subnodes.size(); ++j) {
                        AndEnumerableState state;
                        state.subnodesToIndex.push_back(subnodes[i]);
                        state.subnodesToIndex.push_back(subnodes[j]);
                        andAssignment->choices.push_back(state);
                    }
                }
            }

            // In the simplest case, an AndAssignment picks indices like a PredicateAssignment.  To
            // be indexed we must only pick one index
            //
            // Complications:
            //
            // Some of our child predicates cannot be answered without an index.  As such, the
            // indices that those predicates require must always be outputted.  We store these
            // mandatory index assignments in 'mandatoryIndices'.
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

            // First, add the state of using each subnode.
            for (size_t i = 0; i < subnodes.size(); ++i) {
                AndEnumerableState aes;
                aes.subnodesToIndex.push_back(subnodes[i]);
                andAssignment->choices.push_back(aes);
            }

            // For each FIRST, we assign nodes to it.
            for (IndexToPredMap::iterator it = idxToFirst.begin();
                 it != idxToFirst.end(); ++it) {

                // The assignment we're filling out.
                OneIndexAssignment indexAssign;

                // This is the index we assign to.
                indexAssign.index = it->first;

                const IndexEntry& thisIndex = (*_indices)[it->first];

                // If the index is multikey, we only assign one pred to it.  We also skip
                // compounding.  TODO: is this also true for 2d and 2dsphere indices?  can they be
                // multikey but still compoundable?
                if (thisIndex.multikey) {
                    // TODO: could pick better pred than first but not too worried since we should
                    // really be isecting indices here.  Just take the first pred.  We don't assign
                    // any other preds to this index.  The planner will intersect the preds and this
                    // enumeration strategy is just one index at a time.
                    indexAssign.preds.push_back(it->second[0]);
                    indexAssign.positions.push_back(0);
                }
                else {
                    // The index isn't multikey.  Assign all preds to it.  The planner will
                    // intersect the bounds.
                    indexAssign.preds = it->second;

                    // Since everything in assign.preds prefixes the index, they all go
                    // at position '0' in the index, the first position.
                    indexAssign.positions.resize(indexAssign.preds.size(), 0);

                    // Find everything that could use assign.index but isn't a pred over
                    // the first field of that index.
                    IndexToPredMap::iterator compIt = idxToNotFirst.find(indexAssign.index);
                    if (compIt != idxToNotFirst.end()) {
                        compound(compIt->second, thisIndex, &indexAssign);
                    }
                }

                AndEnumerableState state;
                state.assignments.push_back(indexAssign);
                andAssignment->choices.push_back(state);
            }

            return true;
        }

        // Don't know what the node is at this point.
        return false;
    }

    void PlanEnumerator::compound(const vector<MatchExpression*>& tryCompound,
                                  const IndexEntry& thisIndex,
                                  OneIndexAssignment* assign) {
        // Let's try to match up the expressions in 'compExprs' with the
        // fields in the index key pattern.
        BSONObjIterator kpIt(thisIndex.keyPattern);

        // Skip the first elt as it's already assigned.
        kpIt.next();

        // When we compound we store the field number that the predicate
        // goes over in order to avoid having to iterate again and compare
        // field names.
        size_t posInIdx = 0;

        while (kpIt.more()) {
            BSONElement keyElt = kpIt.next();
            ++posInIdx;
            // We must assign fields contiguously from the left.
            bool fieldAssigned = false;
            for (size_t j = 0; j < tryCompound.size(); ++j) {
                MatchExpression* maybe = tryCompound[j];
                // Sigh we grab the full path from the relevant tag.
                RelevantTag* rt = static_cast<RelevantTag*>(maybe->getTag());
                if (keyElt.fieldName() == rt->path) {
                    // preds and positions are parallel arrays.
                    assign->preds.push_back(maybe);
                    assign->positions.push_back(posInIdx);
                    // We've assigned this field, so we can try the next one.
                    fieldAssigned = true;
                }
            }
            // If we have (a,b,c) and we can't assign something to 'b' don't try
            // to assign something to 'c'.
            if (!fieldAssigned) { break; }
        }
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
        else if (NULL != assign->arrayAssignment) {
            ArrayAssignment* aa = assign->arrayAssignment.get();
            tagMemo(aa->subnodes[aa->counter]);
        }
        else if (NULL != assign->andAssignment) {
            AndAssignment* aa = assign->andAssignment.get();
            verify(aa->counter < aa->choices.size());

            const AndEnumerableState& aes = aa->choices[aa->counter];

            for (size_t j = 0; j < aes.subnodesToIndex.size(); ++j) {
                tagMemo(aes.subnodesToIndex[j]);
            }

            for (size_t i = 0; i < aes.assignments.size(); ++i) {
                const OneIndexAssignment& assign = aes.assignments[i];

                for (size_t j = 0; j < assign.preds.size(); ++j) {
                    MatchExpression* pred = assign.preds[j];
                    verify(NULL == pred->getTag());
                    pred->setTag(new IndexTag(assign.index, assign.positions[j]));
                }
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
        else if (NULL != assign->arrayAssignment) {
            ArrayAssignment* aa = assign->arrayAssignment.get();
            // moving to next on current subnode is OK
            if (!nextMemo(aa->subnodes[aa->counter])) { return false; }
            // Move to next subnode.
            ++aa->counter;
            if (aa->counter < aa->subnodes.size()) {
                return false;
            }
            aa->counter = 0;
            return true;
        }
        else if (NULL != assign->andAssignment) {
            AndAssignment* aa = assign->andAssignment.get();
            ++aa->counter;
            if (aa->counter < aa->choices.size()) {
                return false;
            }
            aa->counter = 0;
            return true;
        }

        // This shouldn't happen.
        verify(0);
        return false;
    }

} // namespace mongo

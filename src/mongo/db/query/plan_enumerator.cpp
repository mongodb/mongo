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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/query/plan_enumerator.h"

#include <set>

#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/indexability.h"
#include "mongo/util/log.h"
#include "mongo/util/string_map.h"

namespace {

using namespace mongo;
using std::unique_ptr;
using std::endl;
using std::set;
using std::string;
using std::vector;

std::string getPathPrefix(std::string path) {
    if (mongoutils::str::contains(path, '.')) {
        return mongoutils::str::before(path, '.');
    } else {
        return path;
    }
}

/**
 * Returns true if either 'node' or a descendent of 'node'
 * is a predicate that is required to use an index.
 */
bool expressionRequiresIndex(const MatchExpression* node) {
    return CanonicalQuery::countNodes(node, MatchExpression::GEO_NEAR) > 0 ||
        CanonicalQuery::countNodes(node, MatchExpression::TEXT) > 0;
}

size_t getPathLength(const MatchExpression* expr) {
    return FieldRef{expr->path()}.numParts();
}

/**
 * Returns true if 'component' refers to a part of 'rt->path' outside the innermost $elemMatch
 * expression, and returns false otherwise. In particular, this function returns false if an
 * expression isn't contained in an $elemMatch.
 *
 * For example, consider the expression {a: {$elemMatch: {b: {$gte: 0, $lt: 10}}}. The path "a.b"
 * (component=1) is inside the $elemMatch expression, whereas the path "a" (component=0) is outside
 * the $elemMatch expression.
 */
bool isPathOutsideElemMatch(const RelevantTag* rt, size_t component) {
    if (rt->elemMatchExpr == nullptr) {
        return false;
    }

    const size_t elemMatchRootLength = getPathLength(rt->elemMatchExpr);
    invariant(elemMatchRootLength > 0);
    return component < elemMatchRootLength;
}

using PossibleFirstAssignment = std::vector<MatchExpression*>;

void getPossibleFirstAssignments(const IndexEntry& thisIndex,
                                 const vector<MatchExpression*>& predsOverLeadingField,
                                 std::vector<PossibleFirstAssignment>* possibleFirstAssignments) {
    invariant(thisIndex.multikey && !thisIndex.multikeyPaths.empty());

    if (thisIndex.multikeyPaths[0].empty()) {
        // No prefix of the leading index field causes the index to be multikey. In other words, the
        // index isn't multikey as a result of the leading index field. We can then safely assign
        // all predicates on it to the index and the access planner will intersect the bounds.
        *possibleFirstAssignments = {predsOverLeadingField};
        return;
    }

    // At least one prefix of the leading index field causes the index to be multikey. We can't
    // intersect bounds on the leading index field unless the predicates are joined by an
    // $elemMatch.
    std::map<MatchExpression*, std::vector<MatchExpression*>> predsByElemMatchExpr;
    for (auto* pred : predsOverLeadingField) {
        invariant(pred->getTag());
        RelevantTag* rt = static_cast<RelevantTag*>(pred->getTag());

        if (rt->elemMatchExpr == nullptr) {
            // 'pred' isn't part of an $elemMatch, so we can't assign any other predicates on the
            // leading index field to the index.
            possibleFirstAssignments->push_back({pred});
        } else {
            // 'pred' is part of an $elemMatch, so we group it together with any other leaf
            // expressions in the same $elemMatch context.
            predsByElemMatchExpr[rt->elemMatchExpr].push_back(pred);
        }
    }

    // We can only assign all of the leaf expressions in the $elemMatch to the index if no prefix of
    // the leading index field that is longer than the root of the $elemMatch causes the index to be
    // multikey. For example, consider the index {'a.b': 1} and the query
    // {a: $elemMatch: {b: {$gte: 0, $lt: 10}}}. If 'a.b' refers to an array value, then the two
    // leaf expressions inside the $elemMatch can match distinct elements. We are therefore unable
    // to assign both to the index and intersect the bounds.
    for (const auto& elemMatchExprIt : predsByElemMatchExpr) {
        invariant(!elemMatchExprIt.second.empty());
        const auto* pred = elemMatchExprIt.second.front();

        invariant(pred->getTag());
        RelevantTag* rt = static_cast<RelevantTag*>(pred->getTag());
        invariant(rt->elemMatchExpr != nullptr);

        const size_t elemMatchRootLength = getPathLength(elemMatchExprIt.first);
        invariant(elemMatchRootLength > 0);

        // Since the multikey path components are 0-indexed, 'elemMatchRootLength' actually
        // corresponds to the path component immediately following the root of the $elemMatch.
        if (thisIndex.multikeyPaths[0].lower_bound(elemMatchRootLength) ==
            thisIndex.multikeyPaths[0].end()) {
            // The root of the $elemMatch is the longest prefix of the leading index field that
            // causes the index to be multikey, so we can assign all of the leaf expressions in the
            // $elemMatch to the index.
            possibleFirstAssignments->push_back(elemMatchExprIt.second);
        } else {
            // There is a path longer than the root of the $elemMatch that causes the index to be
            // multikey, so we can only assign one of the leaf expressions in the $elemMatch to the
            // index. Since we don't know which one is the most selective, we generate a plan for
            // each predicate and rank them against each other.
            for (auto* predCannotIntersect : elemMatchExprIt.second) {
                possibleFirstAssignments->push_back({predCannotIntersect});
            }
        }
    }
}

/**
 * Returns true if the leaf expression associated with 'rt' can be assigned to the index given the
 * path prefixes of the queried field that cause the index to be multikey and the predicates already
 * assigned to the index. Otherwise, this function returns false if the leaf expression associated
 * with 'rt' can't be assigned to the index.
 *
 * This function modifies 'used' under the assumption that if it returns true, then the predicate
 * will be assigned to the index.
 */
bool canAssignPredToIndex(const RelevantTag* rt,
                          const std::set<size_t>& multikeyComponents,
                          StringMap<MatchExpression*>* used) {
    invariant(used);
    const FieldRef path(rt->path);

    // We start by checking with the shortest prefix of the queried path to avoid needing to undo
    // any changes we make to 'used' as we go.
    for (const auto multikeyComponent : multikeyComponents) {
        // 'pathPrefix' is a prefix of a queried path that causes the index to be multikey.
        StringData pathPrefix = path.dottedSubstring(0, multikeyComponent + 1);

        auto search = used->find(pathPrefix);
        if (search == used->end()) {
            // 'pathPrefix' is a prefix of a queried path that we haven't seen before.
            if (isPathOutsideElemMatch(rt, multikeyComponent)) {
                // 'pathPrefix' is outside the innermost $elemMatch, so we record its $elemMatch
                // context to ensure that we don't assign another predicate to 'thisIndex' along
                // this path unless they are part of the same $elemMatch.
                invariant(rt->elemMatchExpr != nullptr);
                (*used)[pathPrefix] = rt->elemMatchExpr;
            } else {
                // 'pathPrefix' is either inside the innermost $elemMatch or not inside an
                // $elemMatch at all. We record that we can't assign another predicate to
                // 'thisIndex' either at or beyond 'pathPrefix' without violating the intersecting
                // and compounding rules for multikey indexes.
                (*used)[pathPrefix] = nullptr;

                // Since we check starting with the shortest prefixes of the queried path that cause
                // 'thisIndex' to be multikey, marking 'used' with nullptr here means that there
                // will be no further attempts to intersect or compound bounds by assigning a
                // different predicate at or beyond 'pathPrefix'.
                break;
            }
        } else {
            // 'pathPrefix' is a prefix of a queried path that we've already assigned to
            // 'thisIndex'. We can only intersect or compound bounds by assigning 'couldAssignPred'
            // to 'thisIndex' if the leaf expressions are joined by the same $elemMatch context.
            const bool cannotAssignPred =
                (search->second == nullptr || search->second != rt->elemMatchExpr);
            if (cannotAssignPred) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace


namespace mongo {

PlanEnumerator::PlanEnumerator(const PlanEnumeratorParams& params)
    : _root(params.root),
      _indices(params.indices),
      _ixisect(params.intersect),
      _orLimit(params.maxSolutionsPerOr),
      _intersectLimit(params.maxIntersectPerAnd) {}

PlanEnumerator::~PlanEnumerator() {
    typedef unordered_map<MemoID, NodeAssignment*> MemoMap;
    for (MemoMap::iterator it = _memo.begin(); it != _memo.end(); ++it) {
        delete it->second;
    }
}

Status PlanEnumerator::init() {
    // Fill out our memo structure from the tagged _root.
    _done = !prepMemo(_root, PrepMemoContext());

    // Dump the tags.  We replace them with IndexTag instances.
    _root->resetTag();

    return Status::OK();
}

std::string PlanEnumerator::dumpMemo() {
    mongoutils::str::stream ss;

    // Note that this needs to be kept in sync with allocateAssignment which assigns memo IDs.
    for (size_t i = 1; i < _memo.size(); ++i) {
        ss << "[Node #" << i << "]: " << _memo[i]->toString() << "\n";
    }
    return ss;
}

string PlanEnumerator::NodeAssignment::toString() const {
    if (NULL != pred) {
        mongoutils::str::stream ss;
        ss << "predicate\n";
        ss << "\tfirst indices: [";
        for (size_t i = 0; i < pred->first.size(); ++i) {
            ss << pred->first[i];
            if (i < pred->first.size() - 1)
                ss << ", ";
        }
        ss << "]\n";
        ss << "\tpred: " << pred->expr->toString();
        ss << "\tindexToAssign: " << pred->indexToAssign;
        return ss;
    } else if (NULL != andAssignment) {
        mongoutils::str::stream ss;
        ss << "AND enumstate counter " << andAssignment->counter;
        for (size_t i = 0; i < andAssignment->choices.size(); ++i) {
            ss << "\n\tchoice " << i << ":\n";
            const AndEnumerableState& state = andAssignment->choices[i];
            ss << "\t\tsubnodes: ";
            for (size_t j = 0; j < state.subnodesToIndex.size(); ++j) {
                ss << state.subnodesToIndex[j] << " ";
            }
            ss << '\n';
            for (size_t j = 0; j < state.assignments.size(); ++j) {
                const OneIndexAssignment& oie = state.assignments[j];
                ss << "\t\tidx[" << oie.index << "]\n";

                for (size_t k = 0; k < oie.preds.size(); ++k) {
                    ss << "\t\t\tpos " << oie.positions[k] << " pred " << oie.preds[k]->toString();
                }
            }
        }
        return ss;
    } else if (NULL != arrayAssignment) {
        mongoutils::str::stream ss;
        ss << "ARRAY SUBNODES enumstate " << arrayAssignment->counter << "/ ONE OF: [ ";
        for (size_t i = 0; i < arrayAssignment->subnodes.size(); ++i) {
            ss << arrayAssignment->subnodes[i] << " ";
        }
        ss << "]";
        return ss;
    } else {
        verify(NULL != orAssignment);
        mongoutils::str::stream ss;
        ss << "ALL OF: [ ";
        for (size_t i = 0; i < orAssignment->subnodes.size(); ++i) {
            ss << orAssignment->subnodes[i] << " ";
        }
        ss << "]";
        return ss;
    }
}

PlanEnumerator::MemoID PlanEnumerator::memoIDForNode(MatchExpression* node) {
    unordered_map<MatchExpression*, MemoID>::iterator it = _nodeToId.find(node);

    if (_nodeToId.end() == it) {
        error() << "Trying to look up memo entry for node, none found.";
        invariant(0);
    }

    return it->second;
}

bool PlanEnumerator::getNext(MatchExpression** tree) {
    if (_done) {
        return false;
    }

    // Tag with our first solution.
    tagMemo(memoIDForNode(_root));

    *tree = _root->shallowClone().release();
    tagForSort(*tree);
    sortUsingTags(*tree);

    _root->resetTag();
    LOG(5) << "Enumerator: memo just before moving:" << endl << dumpMemo();
    _done = nextMemo(memoIDForNode(_root));
    return true;
}

//
// Structure creation
//

void PlanEnumerator::allocateAssignment(MatchExpression* expr,
                                        NodeAssignment** assign,
                                        MemoID* id) {
    // We start at 1 so that the lookup of any entries not explicitly allocated
    // will refer to an invalid memo slot.
    size_t newID = _memo.size() + 1;

    // Shouldn't be anything there already.
    verify(_nodeToId.end() == _nodeToId.find(expr));
    _nodeToId[expr] = newID;
    verify(_memo.end() == _memo.find(newID));
    NodeAssignment* newAssignment = new NodeAssignment();
    _memo[newID] = newAssignment;
    *assign = newAssignment;
    *id = newID;
}

bool PlanEnumerator::prepMemo(MatchExpression* node, PrepMemoContext context) {
    PrepMemoContext childContext;
    childContext.elemMatchExpr = context.elemMatchExpr;
    if (Indexability::nodeCanUseIndexOnOwnField(node)) {
        // We only get here if our parent is an OR, an array operator, or we're the root.

        // If we have no index tag there are no indices we can use.
        if (NULL == node->getTag()) {
            return false;
        }

        RelevantTag* rt = static_cast<RelevantTag*>(node->getTag());
        // In order to definitely use an index it must be prefixed with our field.
        // We don't consider notFirst indices here because we must be AND-related to a node
        // that uses the first spot in that index, and we currently do not know that
        // unless we're in an AND node.
        if (0 == rt->first.size()) {
            return false;
        }

        // We know we can use an index, so grab a memo spot.
        size_t myMemoID;
        NodeAssignment* assign;
        allocateAssignment(node, &assign, &myMemoID);

        assign->pred.reset(new PredicateAssignment());
        assign->pred->expr = node;
        assign->pred->first.swap(rt->first);
        return true;
    } else if (Indexability::isBoundsGeneratingNot(node)) {
        bool childIndexable = prepMemo(node->getChild(0), childContext);
        // If the child isn't indexable then bail out now.
        if (!childIndexable) {
            return false;
        }

        // Our parent node, if any exists, will expect a memo entry keyed on 'node'.  As such we
        // have the node ID for 'node' just point to the memo created for the child that
        // actually generates the bounds.
        size_t myMemoID;
        NodeAssignment* assign;
        allocateAssignment(node, &assign, &myMemoID);
        OrAssignment* orAssignment = new OrAssignment();
        orAssignment->subnodes.push_back(memoIDForNode(node->getChild(0)));
        assign->orAssignment.reset(orAssignment);
        return true;
    } else if (MatchExpression::OR == node->matchType()) {
        // For an OR to be indexed, all its children must be indexed.
        for (size_t i = 0; i < node->numChildren(); ++i) {
            if (!prepMemo(node->getChild(i), childContext)) {
                return false;
            }
        }

        // If we're here we're fully indexed and can be in the memo.
        size_t myMemoID;
        NodeAssignment* assign;
        allocateAssignment(node, &assign, &myMemoID);

        OrAssignment* orAssignment = new OrAssignment();
        for (size_t i = 0; i < node->numChildren(); ++i) {
            orAssignment->subnodes.push_back(memoIDForNode(node->getChild(i)));
        }
        assign->orAssignment.reset(orAssignment);
        return true;
    } else if (Indexability::arrayUsesIndexOnChildren(node)) {
        // Add each of our children as a subnode.  We enumerate through each subnode one at a
        // time until it's exhausted then we move on.
        unique_ptr<ArrayAssignment> aa(new ArrayAssignment());

        if (MatchExpression::ELEM_MATCH_OBJECT == node->matchType()) {
            childContext.elemMatchExpr = node;
        }

        // For an OR to be indexed, all its children must be indexed.
        for (size_t i = 0; i < node->numChildren(); ++i) {
            if (prepMemo(node->getChild(i), childContext)) {
                aa->subnodes.push_back(memoIDForNode(node->getChild(i)));
            }
        }

        if (0 == aa->subnodes.size()) {
            return false;
        }

        size_t myMemoID;
        NodeAssignment* assign;
        allocateAssignment(node, &assign, &myMemoID);

        assign->arrayAssignment.reset(aa.release());
        return true;
    } else if (MatchExpression::AND == node->matchType()) {
        // Map from idx id to children that have a pred over it.

        // TODO: The index intersection logic could be simplified if we could iterate over these
        // maps in a known order. Currently when iterating over these maps we have to impose an
        // ordering on each individual pair of indices in order to make sure that the
        // enumeration results are order-independent. See SERVER-12196.
        IndexToPredMap idxToFirst;
        IndexToPredMap idxToNotFirst;

        // Children that aren't predicates, and which do not necessarily need
        // to use an index.
        vector<MemoID> subnodes;

        // Children that aren't predicates, but which *must* use an index.
        // (e.g. an OR which contains a TEXT child).
        vector<MemoID> mandatorySubnodes;

        // A list of predicates contained in the subtree rooted at 'node'
        // obtained by traversing deeply through $and and $elemMatch children.
        vector<MatchExpression*> indexedPreds;

        // Partition the childen into the children that aren't predicates which may or may
        // not be indexed ('subnodes'), children that aren't predicates which must use the
        // index ('mandatorySubnodes'). and children that are predicates ('indexedPreds').
        //
        // We have to get the subnodes with mandatory assignments rather than adding the
        // mandatory preds to 'indexedPreds'. Adding the mandatory preds directly to
        // 'indexedPreds' would lead to problems such as pulling a predicate beneath an OR
        // into a set joined by an AND.
        if (!partitionPreds(node, childContext, &indexedPreds, &subnodes, &mandatorySubnodes)) {
            return false;
        }

        if (mandatorySubnodes.size() > 1) {
            return false;
        }

        // There can only be one mandatory predicate (at most one $text, at most one
        // $geoNear, can't combine $text/$geoNear).
        MatchExpression* mandatoryPred = NULL;

        // There could be multiple indices which we could use to satisfy the mandatory
        // predicate. Keep the set of such indices. Currently only one text index is
        // allowed per collection, but there could be multiple 2d or 2dsphere indices
        // available to answer a $geoNear predicate.
        set<IndexID> mandatoryIndices;

        // Go through 'indexedPreds' and add the predicates to the
        // 'idxToFirst' and 'idxToNotFirst' maps.
        for (size_t i = 0; i < indexedPreds.size(); ++i) {
            MatchExpression* child = indexedPreds[i];

            invariant(Indexability::nodeCanUseIndexOnOwnField(child));

            RelevantTag* rt = static_cast<RelevantTag*>(child->getTag());

            if (expressionRequiresIndex(child)) {
                // 'child' is a predicate which *must* be tagged with an index.
                // This should include only TEXT and GEO_NEAR preds.

                // We expect either 0 or 1 mandatory predicates.
                invariant(NULL == mandatoryPred);

                // Mandatory predicates are TEXT or GEO_NEAR.
                invariant(MatchExpression::TEXT == child->matchType() ||
                          MatchExpression::GEO_NEAR == child->matchType());

                // The mandatory predicate must have a corresponding "mandatory index".
                invariant(rt->first.size() != 0 || rt->notFirst.size() != 0);

                mandatoryPred = child;

                // Find all of the indices that could be used to satisfy the pred,
                // and add them to the 'mandatoryIndices' set.
                mandatoryIndices.insert(rt->first.begin(), rt->first.end());
                mandatoryIndices.insert(rt->notFirst.begin(), rt->notFirst.end());
            }

            for (size_t j = 0; j < rt->first.size(); ++j) {
                idxToFirst[rt->first[j]].push_back(child);
            }

            for (size_t j = 0; j < rt->notFirst.size(); ++j) {
                idxToNotFirst[rt->notFirst[j]].push_back(child);
            }
        }

        // If none of our children can use indices, bail out.
        if (idxToFirst.empty() && (subnodes.size() == 0) && (mandatorySubnodes.size() == 0)) {
            return false;
        }

        // At least one child can use an index, so we can create a memo entry.
        AndAssignment* andAssignment = new AndAssignment();

        size_t myMemoID;
        NodeAssignment* nodeAssignment;
        allocateAssignment(node, &nodeAssignment, &myMemoID);
        // Takes ownership.
        nodeAssignment->andAssignment.reset(andAssignment);

        // Predicates which must use an index might be buried inside
        // a subnode. Handle that case here.
        if (1 == mandatorySubnodes.size()) {
            AndEnumerableState aes;
            aes.subnodesToIndex.push_back(mandatorySubnodes[0]);
            andAssignment->choices.push_back(aes);
            return true;
        }

        if (NULL != mandatoryPred) {
            // We must have at least one index which can be used to answer 'mandatoryPred'.
            invariant(!mandatoryIndices.empty());
            return enumerateMandatoryIndex(
                idxToFirst, idxToNotFirst, mandatoryPred, mandatoryIndices, andAssignment);
        }

        enumerateOneIndex(idxToFirst, idxToNotFirst, subnodes, andAssignment);

        if (_ixisect) {
            enumerateAndIntersect(idxToFirst, idxToNotFirst, subnodes, andAssignment);
        }

        return true;
    }

    // Don't know what the node is at this point.
    return false;
}

bool PlanEnumerator::enumerateMandatoryIndex(const IndexToPredMap& idxToFirst,
                                             const IndexToPredMap& idxToNotFirst,
                                             MatchExpression* mandatoryPred,
                                             const set<IndexID>& mandatoryIndices,
                                             AndAssignment* andAssignment) {
    // Generate index assignments for each index in 'mandatoryIndices'. We
    // must assign 'mandatoryPred' to one of these indices, but we try all
    // possibilities in 'mandatoryIndices' because some might be better than
    // others for this query.
    for (set<IndexID>::const_iterator indexIt = mandatoryIndices.begin();
         indexIt != mandatoryIndices.end();
         ++indexIt) {
        // We have a predicate which *must* be tagged to use an index.
        // Get the index entry for the index it should use.
        const IndexEntry& thisIndex = (*_indices)[*indexIt];

        // Only text, 2d, and 2dsphere index types should be able to satisfy
        // mandatory predicates.
        invariant(INDEX_TEXT == thisIndex.type || INDEX_2D == thisIndex.type ||
                  INDEX_2DSPHERE == thisIndex.type);

        OneIndexAssignment indexAssign;
        indexAssign.index = *indexIt;

        IndexToPredMap::const_iterator it = idxToFirst.find(*indexIt);
        if (idxToFirst.end() == it) {
            // We don't have any predicate to assign to the leading field of this index.
            // This means that we cannot generate a solution using this index, so we
            // just move on to the next index.
            continue;
        }

        const vector<MatchExpression*>& predsOverLeadingField = it->second;

        if (thisIndex.multikey && !thisIndex.multikeyPaths.empty()) {
            // 2dsphere indexes are the only special index type that should ever have path-level
            // multikey information.
            invariant(INDEX_2DSPHERE == thisIndex.type);

            if (predsOverLeadingField.end() != std::find(predsOverLeadingField.begin(),
                                                         predsOverLeadingField.end(),
                                                         mandatoryPred)) {
                // The mandatory predicate is on the leading field of 'thisIndex'. We assign it to
                // 'thisIndex' and skip assigning any other predicates on the leading field to
                // 'thisIndex' because no additional predicate on the leading field will generate a
                // more efficient data access plan.
                indexAssign.preds.push_back(mandatoryPred);
                indexAssign.positions.push_back(0);

                auto compIt = idxToNotFirst.find(indexAssign.index);
                if (compIt != idxToNotFirst.end()) {
                    // Assign any predicates on the non-leading index fields to 'indexAssign' that
                    // don't violate the intersecting or compounding rules for multikey indexes.
                    assignMultikeySafePredicates(compIt->second, &indexAssign);
                }
            } else {
                // Assign any predicates on the leading index field to 'indexAssign' that don't
                // violate the intersecting rules for multikey indexes.
                assignMultikeySafePredicates(predsOverLeadingField, &indexAssign);

                // Assign the mandatory predicate to 'thisIndex'. Due to how keys are generated for
                // 2dsphere indexes, it is always safe to assign a predicate on a distinct path to
                // 'thisIndex' and compound bounds; an index entry is produced for each combination
                // of unique values along all of the indexed fields, even if they are in separate
                // array elements. See SERVER-23533 for more details.
                compound({mandatoryPred}, thisIndex, &indexAssign);

                auto compIt = idxToNotFirst.find(indexAssign.index);
                if (compIt != idxToNotFirst.end()) {
                    // Copy the predicates on the non-leading index fields and remove
                    // 'mandatoryPred' to avoid assigning it twice to 'thisIndex'.
                    vector<MatchExpression*> predsOverNonLeadingFields = compIt->second;

                    auto mandIt = std::find(predsOverNonLeadingFields.begin(),
                                            predsOverNonLeadingFields.end(),
                                            mandatoryPred);
                    invariant(mandIt != predsOverNonLeadingFields.end());

                    predsOverNonLeadingFields.erase(mandIt);

                    // Assign any predicates on the non-leading index fields to 'indexAssign' that
                    // don't violate the intersecting or compounding rules for multikey indexes.
                    assignMultikeySafePredicates(predsOverNonLeadingFields, &indexAssign);
                }
            }
        } else if (thisIndex.multikey) {
            // Special handling for multikey mandatory indices.
            if (predsOverLeadingField.end() != std::find(predsOverLeadingField.begin(),
                                                         predsOverLeadingField.end(),
                                                         mandatoryPred)) {
                // The mandatory predicate is over the first field of the index. Assign
                // it now.
                indexAssign.preds.push_back(mandatoryPred);
                indexAssign.positions.push_back(0);
            } else {
                // The mandatory pred is notFirst. Assign an arbitrary predicate
                // over the first position.
                invariant(!predsOverLeadingField.empty());
                indexAssign.preds.push_back(predsOverLeadingField[0]);
                indexAssign.positions.push_back(0);

                // Assign the mandatory predicate at the matching position in the compound
                // index. We do this in order to ensure that the mandatory predicate (and not
                // some other predicate over the same position in the compound index) gets
                // assigned.
                //
                // The bad thing that could happen otherwise: A non-mandatory predicate gets
                // chosen by getMultikeyCompoundablePreds(...) instead of 'mandatoryPred'.
                // We would then fail to assign the mandatory predicate, and hence generate
                // a bad data access plan.
                //
                // The mandatory predicate is assigned by calling compound(...) because
                // compound(...) has logic for matching up a predicate with the proper
                // position in the compound index.
                vector<MatchExpression*> mandatoryToCompound;
                mandatoryToCompound.push_back(mandatoryPred);
                compound(mandatoryToCompound, thisIndex, &indexAssign);

                // At this point we have assigned a predicate over the leading field and
                // we have assigned the mandatory predicate to a trailing field.
                //
                // Ex:
                //   Say we have index {a: 1, b: 1, c: "2dsphere", d: 1}. Also suppose that
                //   there is a $near predicate over "c", with additional predicates over
                //   "a", "b", "c", and "d". We will have assigned the $near predicate at
                //   position 2 and a predicate with path "a" at position 0.
            }

            // Compound remaining predicates in a multikey-safe way.
            IndexToPredMap::const_iterator compIt = idxToNotFirst.find(indexAssign.index);
            if (compIt != idxToNotFirst.end()) {
                const vector<MatchExpression*>& couldCompound = compIt->second;
                vector<MatchExpression*> tryCompound;

                getMultikeyCompoundablePreds(indexAssign.preds, couldCompound, &tryCompound);
                if (tryCompound.size()) {
                    compound(tryCompound, thisIndex, &indexAssign);
                }
            }
        } else {
            // For non-multikey, we don't have to do anything too special.
            // Just assign all "first" predicates and try to compound like usual.
            indexAssign.preds = it->second;

            // Since everything in assign.preds prefixes the index, they all go
            // at position '0' in the index, the first position.
            indexAssign.positions.resize(indexAssign.preds.size(), 0);

            // And now we begin compound analysis.

            // Find everything that could use assign.index but isn't a pred over
            // the first field of that index.
            IndexToPredMap::const_iterator compIt = idxToNotFirst.find(indexAssign.index);
            if (compIt != idxToNotFirst.end()) {
                compound(compIt->second, thisIndex, &indexAssign);
            }
        }

        // The mandatory predicate must be assigned.
        invariant(indexAssign.preds.end() !=
                  std::find(indexAssign.preds.begin(), indexAssign.preds.end(), mandatoryPred));

        // Output the assignments for this index.
        AndEnumerableState state;
        state.assignments.push_back(indexAssign);
        andAssignment->choices.push_back(state);
    }

    return andAssignment->choices.size() > 0;
}

void PlanEnumerator::enumerateOneIndex(const IndexToPredMap& idxToFirst,
                                       const IndexToPredMap& idxToNotFirst,
                                       const vector<MemoID>& subnodes,
                                       AndAssignment* andAssignment) {
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
    for (IndexToPredMap::const_iterator it = idxToFirst.begin(); it != idxToFirst.end(); ++it) {
        const IndexEntry& thisIndex = (*_indices)[it->first];

        if (thisIndex.multikey && !thisIndex.multikeyPaths.empty()) {
            // We have path-level information about what causes 'thisIndex' to be multikey and can
            // use this information to get tighter bounds by assigning additional predicates to the
            // index.
            //
            // Depending on the predicates specified and what parts of the leading index field cause
            // the index to be multikey, we may not be able to assign all of predicates to the
            // index. Since we don't know which set of predicates is the most selective, we generate
            // multiple plans and rank them against each other.
            std::vector<PossibleFirstAssignment> possibleFirstAssignments;
            getPossibleFirstAssignments(thisIndex, it->second, &possibleFirstAssignments);

            // Output an assignment for each of the possible assignments on the leading index field.
            for (const auto& firstAssignment : possibleFirstAssignments) {
                OneIndexAssignment indexAssign;
                indexAssign.index = it->first;
                indexAssign.preds = firstAssignment;
                indexAssign.positions.resize(indexAssign.preds.size(), 0);

                auto compIt = idxToNotFirst.find(indexAssign.index);
                if (compIt != idxToNotFirst.end()) {
                    // Assign any predicates on the non-leading index fields to 'indexAssign' that
                    // don't violate the intersecting and compounding rules for multikey indexes.
                    assignMultikeySafePredicates(compIt->second, &indexAssign);
                }

                AndEnumerableState state;
                state.assignments.push_back(indexAssign);
                andAssignment->choices.push_back(state);
            }
        } else if (thisIndex.multikey) {
            // We don't have path-level information about what causes 'thisIndex' to be multikey.
            // We therefore must assume the worst-case scenario: all prefixes of all indexed fields
            // cause the index to be multikey. We therefore can only assign one of the predicates on
            // the leading index field to the index. Since we don't know which one is the most
            // selective, we generate a plan for each predicate and rank them against each other.
            for (auto pred : it->second) {
                OneIndexAssignment indexAssign;
                indexAssign.index = it->first;

                indexAssign.preds.push_back(pred);
                indexAssign.positions.push_back(0);

                // If there are any preds that could possibly be compounded with this
                // index...
                IndexToPredMap::const_iterator compIt = idxToNotFirst.find(indexAssign.index);
                if (compIt != idxToNotFirst.end()) {
                    const vector<MatchExpression*>& couldCompound = compIt->second;
                    vector<MatchExpression*> tryCompound;

                    // ...select the predicates that are safe to compound and try to
                    // compound them.
                    getMultikeyCompoundablePreds(indexAssign.preds, couldCompound, &tryCompound);
                    if (tryCompound.size()) {
                        compound(tryCompound, thisIndex, &indexAssign);
                    }
                }

                // Output the assignment.
                AndEnumerableState state;
                state.assignments.push_back(indexAssign);
                andAssignment->choices.push_back(state);
            }
        } else {
            // The assignment we're filling out.
            OneIndexAssignment indexAssign;

            // This is the index we assign to.
            indexAssign.index = it->first;

            // The index isn't multikey.  Assign all preds to it.  The planner will
            // intersect the bounds.
            indexAssign.preds = it->second;

            // Since everything in assign.preds prefixes the index, they all go
            // at position '0' in the index, the first position.
            indexAssign.positions.resize(indexAssign.preds.size(), 0);

            // Find everything that could use assign.index but isn't a pred over
            // the first field of that index.
            IndexToPredMap::const_iterator compIt = idxToNotFirst.find(indexAssign.index);
            if (compIt != idxToNotFirst.end()) {
                compound(compIt->second, thisIndex, &indexAssign);
            }

            // Output the assignment.
            AndEnumerableState state;
            state.assignments.push_back(indexAssign);
            andAssignment->choices.push_back(state);
        }
    }
}

void PlanEnumerator::enumerateAndIntersect(const IndexToPredMap& idxToFirst,
                                           const IndexToPredMap& idxToNotFirst,
                                           const vector<MemoID>& subnodes,
                                           AndAssignment* andAssignment) {
    // Hardcoded "look at all members of the power set of size 2" search,
    // a.k.a. "consider all pairs of indices".
    //
    // For each unordered pair of indices do the following:
    //   0. Impose an ordering (idx1, idx2) using the key patterns.
    //   (*See note below.)
    //   1. Assign predicates which prefix idx1 to idx1.
    //   2. Add assigned predicates to a set of predicates---the "already
    //   assigned set".
    //   3. Assign predicates which prefix idx2 to idx2, as long as they
    //   been assigned to idx1 already. Add newly assigned predicates to
    //   the "already assigned set".
    //   4. Try to assign predicates to idx1 by compounding.
    //   5. Add any predicates assigned to idx1 by compounding to the
    //   "already assigned set",
    //   6. Try to assign predicates to idx2 by compounding.
    //   7. Determine if we have already assigned all predicates in
    //   the "already assigned set" to a single index. If so, then
    //   don't generate an ixisect solution, as compounding will
    //   be better. Otherwise, output the ixisect assignments.
    //
    // *NOTE on ordering. Suppose we have two indices A and B, and a
    // predicate P1 which is over the prefix of both indices A and B.
    // If we order the indices (A, B) then P1 will get assigned to A,
    // but if we order the indices (B, A) then P1 will get assigned to
    // B. In order to make sure that we get the same result for the unordered
    // pair {A, B} we have to begin by imposing an ordering. As a more concrete
    // example, if we have indices {x: 1, y: 1} and {x: 1, z: 1} with predicate
    // {x: 3}, we want to make sure that {x: 3} gets assigned to the same index
    // irrespective of ordering.

    size_t sizeBefore = andAssignment->choices.size();

    for (IndexToPredMap::const_iterator firstIt = idxToFirst.begin(); firstIt != idxToFirst.end();
         ++firstIt) {
        const IndexEntry& oneIndex = (*_indices)[firstIt->first];

        // 'oneAssign' is used to assign indices and subnodes or to
        // make assignments for the first index when it's multikey.
        // It is NOT used in the inner loop that considers pairs of
        // indices.
        OneIndexAssignment oneAssign;
        oneAssign.index = firstIt->first;
        oneAssign.preds = firstIt->second;
        // Since everything in assign.preds prefixes the index, they all go
        // at position '0' in the index, the first position.
        oneAssign.positions.resize(oneAssign.preds.size(), 0);

        // We create a scan per predicate so if we have >1 predicate we'll already
        // have at least 2 scans (one predicate per scan as the planner can't
        // intersect bounds when the index is multikey), so we stop here.
        if (oneIndex.multikey && oneAssign.preds.size() > 1) {
            oneAssign.canCombineBounds = false;
            // One could imagine an enormous auto-generated $all query with too many clauses to
            // have an ixscan per clause.
            static const size_t kMaxSelfIntersections = 10;
            if (oneAssign.preds.size() > kMaxSelfIntersections) {
                // Only take the first kMaxSelfIntersections preds.
                oneAssign.preds.resize(kMaxSelfIntersections);
                oneAssign.positions.resize(kMaxSelfIntersections);
            }
            AndEnumerableState state;
            state.assignments.push_back(oneAssign);
            andAssignment->choices.push_back(state);
            continue;
        }

        // Output (subnode, firstAssign) pairs.
        for (size_t i = 0; i < subnodes.size(); ++i) {
            AndEnumerableState indexAndSubnode;
            indexAndSubnode.assignments.push_back(oneAssign);
            indexAndSubnode.subnodesToIndex.push_back(subnodes[i]);
            andAssignment->choices.push_back(indexAndSubnode);
            // Limit n^2.
            if (andAssignment->choices.size() - sizeBefore > _intersectLimit) {
                return;
            }
        }

        // Start looking at all other indices to find one that we want to bundle
        // with firstAssign.
        IndexToPredMap::const_iterator secondIt = firstIt;
        secondIt++;
        for (; secondIt != idxToFirst.end(); secondIt++) {
            const IndexEntry& firstIndex = (*_indices)[secondIt->first];
            const IndexEntry& secondIndex = (*_indices)[secondIt->first];

            // Limit n^2.
            if (andAssignment->choices.size() - sizeBefore > _intersectLimit) {
                return;
            }

            // If the other index we're considering is multikey with >1 pred, we don't
            // want to have it as an additional assignment.  Eventually, it1 will be
            // equal to the current value of secondIt and we'll assign every pred for
            // this mapping to the index.
            if (secondIndex.multikey && secondIt->second.size() > 1) {
                continue;
            }

            //
            // Step #0:
            // Impose an ordering (idx1, idx2) using the key patterns.
            //
            IndexToPredMap::const_iterator it1, it2;
            int ordering = firstIndex.keyPattern.woCompare(secondIndex.keyPattern);
            it1 = (ordering > 0) ? firstIt : secondIt;
            it2 = (ordering > 0) ? secondIt : firstIt;
            const IndexEntry& ie1 = (*_indices)[it1->first];
            const IndexEntry& ie2 = (*_indices)[it2->first];

            //
            // Step #1:
            // Assign predicates which prefix firstIndex to firstAssign.
            //
            OneIndexAssignment firstAssign;
            firstAssign.index = it1->first;
            firstAssign.preds = it1->second;
            // Since everything in assign.preds prefixes the index, they all go
            // at position '0' in the index, the first position.
            firstAssign.positions.resize(firstAssign.preds.size(), 0);

            // We keep track of what preds are assigned to indices either because they
            // prefix the index or have been assigned through compounding. We make sure
            // that these predicates DO NOT become additional index assignments.
            // Example: what if firstAssign is the index (x, y) and we're trying to
            // compound? We want to make sure not to compound if the predicate is
            // already assigned to index y.
            set<MatchExpression*> predsAssigned;

            //
            // Step #2:
            // Add indices assigned in 'firstAssign' to 'predsAssigned'.
            //
            for (size_t i = 0; i < firstAssign.preds.size(); ++i) {
                predsAssigned.insert(firstAssign.preds[i]);
            }

            //
            // Step #3:
            // Assign predicates which prefix secondIndex to secondAssign and
            // have not already been assigned to firstAssign. Any newly
            // assigned predicates are added to 'predsAssigned'.
            //
            OneIndexAssignment secondAssign;
            secondAssign.index = it2->first;
            const vector<MatchExpression*>& preds = it2->second;
            for (size_t i = 0; i < preds.size(); ++i) {
                if (predsAssigned.end() == predsAssigned.find(preds[i])) {
                    secondAssign.preds.push_back(preds[i]);
                    secondAssign.positions.push_back(0);
                    predsAssigned.insert(preds[i]);
                }
            }

            // Every predicate that would use this index is already assigned in
            // firstAssign.
            if (0 == secondAssign.preds.size()) {
                continue;
            }

            //
            // Step #4:
            // Compound on firstAssign, if applicable.
            //
            IndexToPredMap::const_iterator firstIndexCompound =
                idxToNotFirst.find(firstAssign.index);

            // Can't compound with multikey indices.
            if (!ie1.multikey && firstIndexCompound != idxToNotFirst.end()) {
                // We must remove any elements of 'predsAssigned' from consideration.
                vector<MatchExpression*> tryCompound;
                const vector<MatchExpression*>& couldCompound = firstIndexCompound->second;
                for (size_t i = 0; i < couldCompound.size(); ++i) {
                    if (predsAssigned.end() == predsAssigned.find(couldCompound[i])) {
                        tryCompound.push_back(couldCompound[i]);
                    }
                }
                if (tryCompound.size()) {
                    compound(tryCompound, ie1, &firstAssign);
                }
            }

            //
            // Step #5:
            // Make sure predicates assigned by compounding in step #4 do not get
            // assigned again.
            //
            for (size_t i = 0; i < firstAssign.preds.size(); ++i) {
                if (predsAssigned.end() == predsAssigned.find(firstAssign.preds[i])) {
                    predsAssigned.insert(firstAssign.preds[i]);
                }
            }

            //
            // Step #6:
            // Compound on firstAssign, if applicable.
            //
            IndexToPredMap::const_iterator secondIndexCompound =
                idxToNotFirst.find(secondAssign.index);

            if (!ie2.multikey && secondIndexCompound != idxToNotFirst.end()) {
                // We must remove any elements of 'predsAssigned' from consideration.
                vector<MatchExpression*> tryCompound;
                const vector<MatchExpression*>& couldCompound = secondIndexCompound->second;
                for (size_t i = 0; i < couldCompound.size(); ++i) {
                    if (predsAssigned.end() == predsAssigned.find(couldCompound[i])) {
                        tryCompound.push_back(couldCompound[i]);
                    }
                }
                if (tryCompound.size()) {
                    compound(tryCompound, ie2, &secondAssign);
                }
            }

            // Add predicates in 'secondAssign' to the set of all assigned predicates.
            for (size_t i = 0; i < secondAssign.preds.size(); ++i) {
                if (predsAssigned.end() == predsAssigned.find(secondAssign.preds[i])) {
                    predsAssigned.insert(secondAssign.preds[i]);
                }
            }

            //
            // Step #7:
            // Make sure we haven't already assigned this set of predicates by compounding.
            // If we have, then bail out for this pair of indices.
            //
            if (alreadyCompounded(predsAssigned, andAssignment)) {
                // There is no need to add either 'firstAssign' or 'secondAssign'
                // to 'andAssignment' in this case because we have already performed
                // assignments to single indices in enumerateOneIndex(...).
                continue;
            }

            // We're done with this particular pair of indices; output
            // the resulting assignments.
            AndEnumerableState state;
            state.assignments.push_back(firstAssign);
            state.assignments.push_back(secondAssign);
            andAssignment->choices.push_back(state);
        }
    }

    // TODO: Do we just want one subnode at a time?  We can use far more than 2 indices at once
    // doing this very easily.  If we want to restrict the # of indices the children use, when
    // we memoize the subtree above we can restrict it to 1 index at a time.  This can get
    // tricky if we want both an intersection and a 1-index memo entry, since our state change
    // is simple and we don't traverse the memo in any targeted way.  Should also verify that
    // having a one-to-many mapping of MatchExpression to MemoID doesn't break anything.  This
    // approach errors on the side of "too much indexing."
    for (size_t i = 0; i < subnodes.size(); ++i) {
        for (size_t j = i + 1; j < subnodes.size(); ++j) {
            AndEnumerableState state;
            state.subnodesToIndex.push_back(subnodes[i]);
            state.subnodesToIndex.push_back(subnodes[j]);
            andAssignment->choices.push_back(state);
        }
    }
}

bool PlanEnumerator::partitionPreds(MatchExpression* node,
                                    PrepMemoContext context,
                                    vector<MatchExpression*>* indexOut,
                                    vector<MemoID>* subnodesOut,
                                    vector<MemoID>* mandatorySubnodes) {
    for (size_t i = 0; i < node->numChildren(); ++i) {
        MatchExpression* child = node->getChild(i);
        if (Indexability::nodeCanUseIndexOnOwnField(child)) {
            RelevantTag* rt = static_cast<RelevantTag*>(child->getTag());
            if (NULL != context.elemMatchExpr) {
                // If we're in an $elemMatch context, store the
                // innermost parent $elemMatch, as well as the
                // inner path prefix.
                rt->elemMatchExpr = context.elemMatchExpr;
                rt->pathPrefix = getPathPrefix(child->path().toString());
            } else {
                // We're not an $elemMatch context, so we should store
                // the prefix of the full path.
                rt->pathPrefix = getPathPrefix(rt->path);
            }

            // Output this as a pred that can use the index.
            indexOut->push_back(child);
        } else if (Indexability::isBoundsGeneratingNot(child)) {
            partitionPreds(child, context, indexOut, subnodesOut, mandatorySubnodes);
        } else if (MatchExpression::ELEM_MATCH_OBJECT == child->matchType()) {
            PrepMemoContext childContext;
            childContext.elemMatchExpr = child;
            partitionPreds(child, childContext, indexOut, subnodesOut, mandatorySubnodes);
        } else if (MatchExpression::AND == child->matchType()) {
            partitionPreds(child, context, indexOut, subnodesOut, mandatorySubnodes);
        } else {
            bool mandatory = expressionRequiresIndex(child);

            // Recursively prepMemo for the subnode. We fall through
            // to this case for logical nodes other than AND (e.g. OR).
            if (prepMemo(child, context)) {
                size_t childID = memoIDForNode(child);

                // Output the subnode.
                if (mandatory) {
                    mandatorySubnodes->push_back(childID);
                } else {
                    subnodesOut->push_back(childID);
                }
            } else if (mandatory) {
                // The subnode is mandatory but cannot be indexed. This means
                // that the entire AND cannot be indexed either.
                return false;
            }
        }
    }

    return true;
}

void PlanEnumerator::getMultikeyCompoundablePreds(const vector<MatchExpression*>& assigned,
                                                  const vector<MatchExpression*>& couldCompound,
                                                  vector<MatchExpression*>* out) {
    // Map from a particular $elemMatch expression to the set of prefixes
    // used so far by the predicates inside the $elemMatch. For example,
    // {a: {$elemMatch: {b: 1, c: 2}}} would map to the set {'b', 'c'} at
    // the end of this function's execution.
    //
    // NULL maps to the set of prefixes used so far outside of an $elemMatch
    // context.
    //
    // As we iterate over the available indexed predicates, we keep track
    // of the used prefixes both inside and outside of an $elemMatch context.
    unordered_map<MatchExpression*, set<string>> used;

    // Initialize 'used' with the starting predicates in 'assigned'. Begin by
    // initializing the top-level scope with the prefix of the full path.
    for (size_t i = 0; i < assigned.size(); i++) {
        const MatchExpression* assignedPred = assigned[i];
        invariant(NULL != assignedPred->getTag());
        RelevantTag* usedRt = static_cast<RelevantTag*>(assignedPred->getTag());
        set<string> usedPrefixes;
        usedPrefixes.insert(getPathPrefix(usedRt->path));
        used[NULL] = usedPrefixes;

        // If 'assigned' is a predicate inside an $elemMatch, we have to
        // add the prefix not only to the top-level context, but also to the
        // the $elemMatch context. For example, if 'assigned' is {a: {$elemMatch: {b: 1}}},
        // then we will have already added "a" to the set for NULL. We now
        // also need to add "b" to the set for the $elemMatch.
        if (NULL != usedRt->elemMatchExpr) {
            set<string> elemMatchUsed;
            // Whereas getPathPrefix(usedRt->path) is the prefix of the full path,
            // usedRt->pathPrefix contains the prefix of the portion of the
            // path that is inside the $elemMatch. These two prefixes are the same
            // in the top-level context, but here must be different because 'usedRt'
            // is in an $elemMatch context.
            elemMatchUsed.insert(usedRt->pathPrefix);
            used[usedRt->elemMatchExpr] = elemMatchUsed;
        }
    }

    for (size_t i = 0; i < couldCompound.size(); ++i) {
        invariant(Indexability::nodeCanUseIndexOnOwnField(couldCompound[i]));
        RelevantTag* rt = static_cast<RelevantTag*>(couldCompound[i]->getTag());

        if (used.end() == used.find(rt->elemMatchExpr)) {
            // This is a new $elemMatch that we haven't seen before.
            invariant(used.end() != used.find(NULL));
            set<string>& topLevelUsed = used.find(NULL)->second;

            // If the top-level path prefix of the $elemMatch hasn't been
            // used yet, couldCompound[i] is safe to compound.
            if (topLevelUsed.end() == topLevelUsed.find(getPathPrefix(rt->path))) {
                topLevelUsed.insert(getPathPrefix(rt->path));
                set<string> usedPrefixes;
                usedPrefixes.insert(rt->pathPrefix);
                used[rt->elemMatchExpr] = usedPrefixes;

                // Output the predicate.
                out->push_back(couldCompound[i]);
            }

        } else {
            // We've seen this $elemMatch before, or the predicate is
            // top-level (not in an $elemMatch context). If the prefix stored
            // in the tag has not been used yet, then couldCompound[i] is
            // safe to compound.
            set<string>& usedPrefixes = used.find(rt->elemMatchExpr)->second;
            if (usedPrefixes.end() == usedPrefixes.find(rt->pathPrefix)) {
                usedPrefixes.insert(rt->pathPrefix);

                // Output the predicate.
                out->push_back(couldCompound[i]);
            }
        }
    }
}

void PlanEnumerator::assignMultikeySafePredicates(const std::vector<MatchExpression*>& couldAssign,
                                                  OneIndexAssignment* indexAssignment) {
    invariant(indexAssignment);
    invariant(indexAssignment->preds.size() == indexAssignment->positions.size());

    const IndexEntry& thisIndex = (*_indices)[indexAssignment->index];
    invariant(!thisIndex.multikeyPaths.empty());

    // 'used' is a map from each prefix of a queried path that causes 'thisIndex' to be multikey to
    // the 'elemMatchExpr' of the associated leaf expression's RelevantTag. We use it to ensure that
    // leaf expressions sharing a prefix of their queried paths are only both assigned to
    // 'thisIndex' if they are joined by the same $elemMatch context.
    StringMap<MatchExpression*> used;

    // Initialize 'used' with the predicates already assigned to 'thisIndex'.
    for (size_t i = 0; i < indexAssignment->preds.size(); ++i) {
        const auto* assignedPred = indexAssignment->preds[i];
        const auto posInIdx = indexAssignment->positions[i];

        invariant(assignedPred->getTag());
        RelevantTag* rt = static_cast<RelevantTag*>(assignedPred->getTag());

        // 'assignedPred' has already been assigned to 'thisIndex', so canAssignPredToIndex() ought
        // to return true.
        const bool shouldHaveAssigned =
            canAssignPredToIndex(rt, thisIndex.multikeyPaths[posInIdx], &used);
        if (!shouldHaveAssigned) {
            // However, there are cases with multikey 2dsphere indexes where the mandatory predicate
            // is still safe to compound with, even though a prefix of it that causes the index to
            // be multikey can be shared with the leading index field. The predicates cannot
            // possibly be joined by an $elemMatch because $near predicates must be specified at the
            // top-level of the query.
            invariant(assignedPred->matchType() == MatchExpression::GEO_NEAR);
        }
    }

    size_t posInIdx = 0;

    for (const auto keyElem : thisIndex.keyPattern) {
        // Attempt to assign the predicates to 'thisIndex' according to their position in the index
        // key pattern.
        for (auto* couldAssignPred : couldAssign) {
            invariant(Indexability::nodeCanUseIndexOnOwnField(couldAssignPred));
            RelevantTag* rt = static_cast<RelevantTag*>(couldAssignPred->getTag());

            if (keyElem.fieldNameStringData() != rt->path) {
                continue;
            }

            if (thisIndex.multikeyPaths[posInIdx].empty()) {
                // We can always intersect or compound the bounds when no prefix of the queried path
                // causes the index to be multikey.
                indexAssignment->preds.push_back(couldAssignPred);
                indexAssignment->positions.push_back(posInIdx);
                continue;
            }

            // See if any of the predicates that are already assigned to 'thisIndex' prevent us from
            // assigning 'couldAssignPred' as well.
            const bool shouldAssign =
                canAssignPredToIndex(rt, thisIndex.multikeyPaths[posInIdx], &used);

            if (shouldAssign) {
                indexAssignment->preds.push_back(couldAssignPred);
                indexAssignment->positions.push_back(posInIdx);
            }
        }

        ++posInIdx;
    }
}

bool PlanEnumerator::alreadyCompounded(const set<MatchExpression*>& ixisectAssigned,
                                       const AndAssignment* andAssignment) {
    for (size_t i = 0; i < andAssignment->choices.size(); ++i) {
        const AndEnumerableState& state = andAssignment->choices[i];

        // We cannot have assigned this set of predicates already by
        // compounding unless this is an assignment to a single index.
        if (state.assignments.size() != 1) {
            continue;
        }

        // If the set of preds in 'ixisectAssigned' is a subset of 'oneAssign.preds',
        // then all the preds can be used by compounding on a single index.
        const OneIndexAssignment& oneAssign = state.assignments[0];

        // If 'ixisectAssigned' is larger than 'oneAssign.preds', then
        // it can't be a subset.
        if (ixisectAssigned.size() > oneAssign.preds.size()) {
            continue;
        }

        // Check for subset by counting the number of elements in 'oneAssign.preds'
        // that are contained in 'ixisectAssigned'. The elements of both 'oneAssign.preds'
        // and 'ixisectAssigned' are unique (no repeated elements).
        size_t count = 0;
        for (size_t j = 0; j < oneAssign.preds.size(); ++j) {
            if (ixisectAssigned.end() != ixisectAssigned.find(oneAssign.preds[j])) {
                ++count;
            }
        }

        if (ixisectAssigned.size() == count) {
            return true;
        }

        // We cannot assign the preds by compounding on 'oneAssign'.
        // Move on to the next index.
    }

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

        // Go through 'tryCompound' to see if there is a compoundable
        // predicate for 'keyElt'. If there is nothing to compound, then
        // simply move on to the next field in the compound index. We
        // do not enforce that fields are assigned contiguously from
        // right to left, i.e. for compound index {a: 1, b: 1, c: 1}
        // it is okay to compound predicates over "a" and "c", skipping "b".
        for (size_t j = 0; j < tryCompound.size(); ++j) {
            MatchExpression* maybe = tryCompound[j];
            // Sigh we grab the full path from the relevant tag.
            RelevantTag* rt = static_cast<RelevantTag*>(maybe->getTag());
            if (keyElt.fieldName() == rt->path) {
                // preds and positions are parallel arrays.
                assign->preds.push_back(maybe);
                assign->positions.push_back(posInIdx);
            }
        }
    }
}

//
// Structure navigation
//

void PlanEnumerator::tagMemo(size_t id) {
    LOG(5) << "Tagging memoID " << id << endl;
    NodeAssignment* assign = _memo[id];
    verify(NULL != assign);

    if (NULL != assign->pred) {
        PredicateAssignment* pa = assign->pred.get();
        verify(NULL == pa->expr->getTag());
        verify(pa->indexToAssign < pa->first.size());
        pa->expr->setTag(new IndexTag(pa->first[pa->indexToAssign]));
    } else if (NULL != assign->orAssignment) {
        OrAssignment* oa = assign->orAssignment.get();
        for (size_t i = 0; i < oa->subnodes.size(); ++i) {
            tagMemo(oa->subnodes[i]);
        }
    } else if (NULL != assign->arrayAssignment) {
        ArrayAssignment* aa = assign->arrayAssignment.get();
        tagMemo(aa->subnodes[aa->counter]);
    } else if (NULL != assign->andAssignment) {
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
                pred->setTag(
                    new IndexTag(assign.index, assign.positions[j], assign.canCombineBounds));
            }
        }
    } else {
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
    } else if (NULL != assign->orAssignment) {
        OrAssignment* oa = assign->orAssignment.get();

        // Limit the number of OR enumerations
        oa->counter++;
        if (oa->counter >= _orLimit) {
            return true;
        }

        // OR just walks through telling its children to
        // move forward.
        for (size_t i = 0; i < oa->subnodes.size(); ++i) {
            // If there's no carry, we just stop.  If there's a carry, we move the next child
            // forward.
            if (!nextMemo(oa->subnodes[i])) {
                return false;
            }
        }
        // If we're here, the last subnode had a carry, therefore the OR has a carry.
        return true;
    } else if (NULL != assign->arrayAssignment) {
        ArrayAssignment* aa = assign->arrayAssignment.get();
        // moving to next on current subnode is OK
        if (!nextMemo(aa->subnodes[aa->counter])) {
            return false;
        }
        // Move to next subnode.
        ++aa->counter;
        if (aa->counter < aa->subnodes.size()) {
            return false;
        }
        aa->counter = 0;
        return true;
    } else if (NULL != assign->andAssignment) {
        AndAssignment* aa = assign->andAssignment.get();

        // One of our subnodes might have to move on to its next enumeration state.
        const AndEnumerableState& aes = aa->choices[aa->counter];
        for (size_t i = 0; i < aes.subnodesToIndex.size(); ++i) {
            if (!nextMemo(aes.subnodesToIndex[i])) {
                return false;
            }
        }

        // None of the subnodes had another enumeration state, so we move on to the
        // next top-level choice.
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

}  // namespace mongo

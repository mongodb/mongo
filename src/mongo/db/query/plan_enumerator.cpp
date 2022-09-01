/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/query/plan_enumerator.h"

#include <set>

#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/indexability.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace {

using namespace mongo;
using std::endl;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

std::string getPathPrefix(std::string path) {
    if (auto dot = path.find('.'); dot != path.npos)
        path.resize(dot);
    return path;
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
                          const MultikeyComponents& multikeyComponents,
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

/**
 * Tags each node of the tree with the lowest numbered index that the sub-tree rooted at that
 * node uses.
 *
 * Nodes that satisfy Indexability::nodeCanUseIndexOnOwnField are already tagged if there
 * exists an index that that node can use.
 */
void tagForSort(MatchExpression* tree) {
    if (!Indexability::nodeCanUseIndexOnOwnField(tree)) {
        const IndexTag* myIndexTag = nullptr;
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            MatchExpression* child = tree->getChild(i);
            tagForSort(child);
            if (child->getTag() &&
                child->getTag()->getType() == MatchExpression::TagData::Type::IndexTag) {
                auto childTag = static_cast<const IndexTag*>(child->getTag());
                if (!myIndexTag || myIndexTag->index > childTag->index) {
                    myIndexTag = childTag;
                }
            } else if (child->getTag() &&
                       child->getTag()->getType() ==
                           MatchExpression::TagData::Type::OrPushdownTag) {
                OrPushdownTag* childTag = static_cast<OrPushdownTag*>(child->getTag());
                if (childTag->getIndexTag()) {
                    auto indexTag = static_cast<const IndexTag*>(childTag->getIndexTag());
                    if (!myIndexTag || myIndexTag->index > indexTag->index) {
                        myIndexTag = indexTag;
                    }
                }
            }
        }
        if (myIndexTag) {
            tree->setTag(new IndexTag(*myIndexTag));
        }
    }
}

}  // namespace


namespace mongo {

PlanEnumerator::PlanEnumerator(const PlanEnumeratorParams& params)
    : _root(params.root),
      _indices(params.indices),
      _ixisect(params.intersect),
      _enumerateOrChildrenLockstep(params.enumerateOrChildrenLockstep),
      _orLimit(params.maxSolutionsPerOr),
      _intersectLimit(params.maxIntersectPerAnd) {}

PlanEnumerator::~PlanEnumerator() {
    typedef stdx::unordered_map<MemoID, NodeAssignment*> MemoMap;
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
    str::stream ss;

    // Note that this needs to be kept in sync with allocateAssignment which assigns memo IDs.
    for (size_t i = 1; i <= _memo.size(); ++i) {
        ss << "[Node #" << i << "]: " << _memo[i]->toString() << "\n";
    }
    return ss;
}

string PlanEnumerator::NodeAssignment::toString() const {
    if (nullptr != andAssignment) {
        str::stream ss;
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
                    ss << "\t\t\tpos " << oie.positions[k] << " pred "
                       << oie.preds[k]->debugString();
                }

                for (auto&& pushdown : oie.orPushdowns) {
                    ss << "\t\torPushdownPred: " << pushdown.first->debugString();
                }
            }
        }
        return ss;
    } else if (nullptr != arrayAssignment) {
        str::stream ss;
        ss << "ARRAY SUBNODES enumstate " << arrayAssignment->counter << "/ ONE OF: [ ";
        for (size_t i = 0; i < arrayAssignment->subnodes.size(); ++i) {
            ss << arrayAssignment->subnodes[i] << " ";
        }
        ss << "]";
        return ss;
    } else if (nullptr != orAssignment) {
        str::stream ss;
        ss << "ALL OF: [ ";
        for (size_t i = 0; i < orAssignment->subnodes.size(); ++i) {
            ss << orAssignment->subnodes[i] << " ";
        }
        ss << "]";
        return ss;
    } else if (nullptr != lockstepOrAssignment) {
        str::stream ss;
        ss << "ALL OF (lockstep): {";
        ss << "\n\ttotalEnumerated: " << lockstepOrAssignment->totalEnumerated;
        ss << "\n\tsubnodes: [ ";
        for (auto&& node : lockstepOrAssignment->subnodes) {
            ss << "\n\t\t{";
            ss << "memoId: " << node.memoId << ", ";
            ss << "iterationCount: " << node.iterationCount << ", ";
            if (node.maxIterCount) {
                ss << "maxIterCount: " << node.maxIterCount;
            } else {
                ss << "maxIterCount: none";
            }
            ss << "},";
        }
        ss << "\n]";
        return ss;
    }
    MONGO_UNREACHABLE;
}

PlanEnumerator::MemoID PlanEnumerator::memoIDForNode(MatchExpression* node) {
    stdx::unordered_map<MatchExpression*, MemoID>::iterator it = _nodeToId.find(node);

    if (_nodeToId.end() == it) {
        LOGV2_ERROR(20945, "Trying to look up memo entry for node, none found");
        MONGO_UNREACHABLE;
    }

    return it->second;
}

unique_ptr<MatchExpression> PlanEnumerator::getNext() {
    if (_done) {
        return nullptr;
    }

    // Tag with our first solution.
    tagMemo(memoIDForNode(_root));

    unique_ptr<MatchExpression> tree(_root->shallowClone());
    tagForSort(tree.get());

    _root->resetTag();
    LOGV2_DEBUG(20943, 5, "Enumerator: memo just before moving", "memo"_attr = dumpMemo());
    _done = nextMemo(memoIDForNode(_root));
    return tree;
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
    childContext.outsidePreds = context.outsidePreds;

    if (MatchExpression::OR == node->matchType()) {
        if (_orLimit == 0) {
            LOGV2_DEBUG(4862501,
                        1,
                        "plan enumerator exceeded threshold for OR enumerations",
                        "orEnumerationLimit"_attr = _orLimit);
            _explainInfo.hitIndexedOrLimit = true;
            return false;
        }
        // For an OR to be indexed, all its children must be indexed.
        for (size_t i = 0; i < node->numChildren(); ++i) {

            // Extend the path through the indexed ORs of each outside predicate.
            auto childContextCopy = childContext;
            for (auto it = childContextCopy.outsidePreds.begin();
                 it != childContextCopy.outsidePreds.end();) {
                // If the route has already traversed through an $elemMatch object, then we cannot
                // push down through this OR. Here we remove such routes from our context object.
                //
                // For example, suppose we have index {a: 1, "b.c": 1} and the following query:
                //
                //   {a: 1, b: {$elemMatch: {$or: [{c: 2}, {c: 3}]}}}
                //
                // It is not correct to push the 'a' predicate down such that it is a sibling of
                // either of the predicates on 'c', since this would change the predicate's meaning
                // from a==1 to "b.a"==1.
                if (it->second.traversedThroughElemMatchObj) {
                    childContextCopy.outsidePreds.erase(it++);
                } else {
                    it->second.route.push_back(i);
                    ++it;
                }
            }

            if (!prepMemo(node->getChild(i), childContextCopy)) {
                return false;
            }
        }

        // If we're here we're fully indexed and can be in the memo.
        size_t myMemoID;
        NodeAssignment* assign;
        allocateAssignment(node, &assign, &myMemoID);

        if (_enumerateOrChildrenLockstep) {
            LockstepOrAssignment* newOrAssign = new LockstepOrAssignment();
            for (size_t i = 0; i < node->numChildren(); ++i) {
                newOrAssign->subnodes.push_back({memoIDForNode(node->getChild(i)), 0, boost::none});
            }
            assign->lockstepOrAssignment.reset(newOrAssign);
        } else {
            OrAssignment* orAssignment = new OrAssignment();
            for (size_t i = 0; i < node->numChildren(); ++i) {
                orAssignment->subnodes.push_back(memoIDForNode(node->getChild(i)));
            }
            assign->orAssignment.reset(orAssignment);
        }
        return true;
    } else if (Indexability::arrayUsesIndexOnChildren(node)) {
        // Add each of our children as a subnode.  We enumerate through each subnode one at a
        // time until it's exhausted then we move on.
        unique_ptr<ArrayAssignment> aa(new ArrayAssignment());

        if (MatchExpression::ELEM_MATCH_OBJECT == node->matchType()) {
            childContext.elemMatchExpr = node;
            markTraversedThroughElemMatchObj(&childContext);
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

        assign->arrayAssignment = std::move(aa);
        return true;
    } else if (Indexability::nodeCanUseIndexOnOwnField(node) ||
               Indexability::isBoundsGeneratingNot(node) ||
               (MatchExpression::AND == node->matchType())) {
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

        // A list of predicates contained in the subtree rooted at 'node' obtained by traversing
        // deeply through $and and $elemMatch children.
        std::vector<MatchExpression*> indexedPreds;

        // Partition the childen into the children that aren't predicates which may or may not be
        // indexed ('subnodes'), children that aren't predicates which must use the index
        // ('mandatorySubnodes'). and children that are predicates ('indexedPreds').
        //
        // We have to get the subnodes with mandatory assignments rather than adding the mandatory
        // preds to 'indexedPreds'. Adding the mandatory preds directly to 'indexedPreds' would lead
        // to problems such as pulling a predicate beneath an OR into a set joined by an AND.
        getIndexedPreds(node, childContext, &indexedPreds);
        // Pass in the indexed predicates as outside predicates when prepping the subnodes.
        auto childContextCopy = childContext;
        for (auto pred : indexedPreds) {
            childContextCopy.outsidePreds[pred] = OutsidePredRoute{};
        }
        if (!prepSubNodes(node, childContextCopy, &subnodes, &mandatorySubnodes)) {
            return false;
        }

        if (mandatorySubnodes.size() > 1) {
            return false;
        }

        // There can only be one mandatory predicate (at most one $text, at most one
        // $geoNear, can't combine $text/$geoNear).
        MatchExpression* mandatoryPred = nullptr;

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
                invariant(nullptr == mandatoryPred);

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
        if (idxToFirst.empty() && idxToNotFirst.empty() && (subnodes.size() == 0) &&
            (mandatorySubnodes.size() == 0)) {
            return false;
        }

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
            andAssignment->choices.push_back(std::move(aes));
            return true;
        }

        if (nullptr != mandatoryPred) {
            // We must have at least one index which can be used to answer 'mandatoryPred'.
            invariant(!mandatoryIndices.empty());
            return enumerateMandatoryIndex(
                idxToFirst, idxToNotFirst, mandatoryPred, mandatoryIndices, andAssignment);
        }

        enumerateOneIndex(
            idxToFirst, idxToNotFirst, subnodes, childContext.outsidePreds, andAssignment);

        if (_ixisect) {
            enumerateAndIntersect(idxToFirst, idxToNotFirst, subnodes, andAssignment);
        }

        return !andAssignment->choices.empty();
    }

    // Don't know what the node is at this point.
    return false;
}

void PlanEnumerator::assignToNonMultikeyMandatoryIndex(
    const IndexEntry& index,
    const std::vector<MatchExpression*>& predsOverLeadingField,
    const IndexToPredMap& idxToNotFirst,
    OneIndexAssignment* indexAssign) {
    // Text indexes are typically multikey because there is an index key for each token in the
    // source text. However, the leading and trailing non-text fields of the index cannot be
    // multikey. As a result, we should use non-multikey predicate assignment rules for such
    // indexes.
    invariant(!index.multikey || index.type == IndexType::INDEX_TEXT);

    // Since the index is not multikey, all predicates over the leading field can be assigned.
    indexAssign->preds = predsOverLeadingField;

    // Since everything in assign.preds prefixes the index, they all go at position '0' in the
    // index, the first position.
    indexAssign->positions.resize(indexAssign->preds.size(), 0);

    // And now we begin compound analysis. Find everything that could use assign.index but isn't a
    // pred over the first field of that index.
    auto compIt = idxToNotFirst.find(indexAssign->index);
    if (compIt != idxToNotFirst.end()) {
        compound(compIt->second, index, indexAssign);
    }
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

        // Text indexes should be treated like non-multikey indexes, since the non-text fields are
        // prohibited from containing arrays.
        if (thisIndex.type == IndexType::INDEX_TEXT) {
            assignToNonMultikeyMandatoryIndex(
                thisIndex, predsOverLeadingField, idxToNotFirst, &indexAssign);
        } else if (thisIndex.multikey && !thisIndex.multikeyPaths.empty()) {
            // 2dsphere indexes are the only special index type that should ever have path-level
            // multikey information.
            invariant(INDEX_2DSPHERE == thisIndex.type);

            if (predsOverLeadingField.end() !=
                std::find(
                    predsOverLeadingField.begin(), predsOverLeadingField.end(), mandatoryPred)) {
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
                    // We do not currently try to assign outside predicates to mandatory indexes.
                    const stdx::unordered_map<MatchExpression*, OutsidePredRoute> outsidePreds{};
                    assignMultikeySafePredicates(compIt->second, outsidePreds, &indexAssign);
                }
            } else {
                // Assign any predicates on the leading index field to 'indexAssign' that don't
                // violate the intersecting rules for multikey indexes.
                // We do not currently try to assign outside predicates to mandatory indexes.
                const stdx::unordered_map<MatchExpression*, OutsidePredRoute> outsidePreds{};
                assignMultikeySafePredicates(predsOverLeadingField, outsidePreds, &indexAssign);

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
                    // We do not currently try to assign outside predicates to mandatory indexes.
                    assignMultikeySafePredicates(
                        predsOverNonLeadingFields, outsidePreds, &indexAssign);
                }
            }
        } else if (thisIndex.multikey) {
            // Special handling for multikey mandatory indices.
            if (predsOverLeadingField.end() !=
                std::find(
                    predsOverLeadingField.begin(), predsOverLeadingField.end(), mandatoryPred)) {
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
            // The index is not multikey.
            assignToNonMultikeyMandatoryIndex(
                thisIndex, predsOverLeadingField, idxToNotFirst, &indexAssign);
        }

        // The mandatory predicate must be assigned.
        invariant(indexAssign.preds.end() !=
                  std::find(indexAssign.preds.begin(), indexAssign.preds.end(), mandatoryPred));

        // Output the assignments for this index.
        AndEnumerableState state;
        state.assignments.push_back(std::move(indexAssign));
        andAssignment->choices.push_back(std::move(state));
    }

    return andAssignment->choices.size() > 0;
}

void PlanEnumerator::assignPredicate(
    const stdx::unordered_map<MatchExpression*, OutsidePredRoute>& outsidePreds,
    MatchExpression* pred,
    size_t position,
    OneIndexAssignment* indexAssignment) {
    if (outsidePreds.find(pred) != outsidePreds.end()) {
        OrPushdownTag::Destination dest;
        dest.route = outsidePreds.at(pred).route;

        // This method should only be called if we can combine bounds.
        const bool canCombineBounds = true;
        dest.tagData =
            std::make_unique<IndexTag>(indexAssignment->index, position, canCombineBounds);
        indexAssignment->orPushdowns.emplace_back(pred, std::move(dest));
    } else {
        indexAssignment->preds.push_back(pred);
        indexAssignment->positions.push_back(position);
    }
}

void PlanEnumerator::markTraversedThroughElemMatchObj(PrepMemoContext* context) {
    invariant(context);
    for (auto&& pred : context->outsidePreds) {
        auto relevantTag = static_cast<RelevantTag*>(pred.first->getTag());
        // Only indexed predicates should ever be considered as outside predicates eligible for
        // pushdown.
        invariant(relevantTag);

        // Check whether the current $elemMatch through which we are traversing is the same as the
        // outside predicate's $elemMatch context. If so, then that outside predicate hasn't
        // actually traversed through an $elemMatch (it has simply been promoted by
        // getIndexedPreds() into the set of AND-related indexed predicates). If not, then the OR
        // pushdown route descends through an $elemMatch object node, and must be marked as such.
        if (relevantTag->elemMatchExpr != context->elemMatchExpr) {
            pred.second.traversedThroughElemMatchObj = true;
        }
    }
}

void PlanEnumerator::enumerateOneIndex(
    IndexToPredMap idxToFirst,
    IndexToPredMap idxToNotFirst,
    const vector<MemoID>& subnodes,
    const stdx::unordered_map<MatchExpression*, OutsidePredRoute>& outsidePreds,
    AndAssignment* andAssignment) {
    // Each choice in the 'andAssignment' will consist of a single subnode to index (an OR or array
    // operator) or a OneIndexAssignment. When creating a OneIndexAssignment, we ensure that at
    // least one predicate can fulfill the first position in the key pattern, then we assign all
    // predicates that can use the key pattern to the index. However, if the index is multikey,
    // certain predicates cannot be combined/compounded. We determine which predicates can be
    // combined/compounded using path-level multikey info, if available.

    // First, add the state of using each subnode.
    for (size_t i = 0; i < subnodes.size(); ++i) {
        AndEnumerableState aes;
        aes.subnodesToIndex.push_back(subnodes[i]);
        andAssignment->choices.push_back(std::move(aes));
    }

    // Next we create OneIndexAssignments.

    // If there are any 'outsidePreds', then we are in a contained OR, and the 'outsidePreds' are
    // AND-related to the contained OR and can be pushed inside of it. Add all of the 'outsidePreds'
    // to 'idxToFirst' and 'idxToNotFirst'. We will treat them as normal predicates that can be
    // assigned to the index, but we will ensure that any OneIndexAssignment contains some
    // predicates from the current node.
    for (const auto& pred : outsidePreds) {
        invariant(pred.first->getTag());
        RelevantTag* relevantTag = static_cast<RelevantTag*>(pred.first->getTag());
        for (auto index : relevantTag->first) {
            if (idxToFirst.find(index) != idxToFirst.end() ||
                idxToNotFirst.find(index) != idxToNotFirst.end()) {
                idxToFirst[index].push_back(pred.first);
            }
        }
        for (auto index : relevantTag->notFirst) {
            if (idxToFirst.find(index) != idxToFirst.end() ||
                idxToNotFirst.find(index) != idxToNotFirst.end()) {
                idxToNotFirst[index].push_back(pred.first);
            }
        }
    }

    // For each FIRST, we assign predicates to it.
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

                for (auto pred : firstAssignment) {
                    assignPredicate(outsidePreds, pred, 0, &indexAssign);
                }

                auto compIt = idxToNotFirst.find(indexAssign.index);
                if (compIt != idxToNotFirst.end()) {
                    // Assign any predicates on the non-leading index fields to 'indexAssign' that
                    // don't violate the intersecting and compounding rules for multikey indexes.
                    assignMultikeySafePredicates(compIt->second, outsidePreds, &indexAssign);
                }

                // Do not output this assignment if it consists only of outside predicates.
                if (!indexAssign.preds.empty()) {
                    AndEnumerableState state;
                    state.assignments.push_back(std::move(indexAssign));
                    andAssignment->choices.push_back(std::move(state));
                }
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

                assignPredicate(outsidePreds, pred, 0, &indexAssign);

                // If there are any preds that could possibly be compounded with this
                // index...
                IndexToPredMap::const_iterator compIt = idxToNotFirst.find(indexAssign.index);
                if (compIt != idxToNotFirst.end()) {
                    const vector<MatchExpression*>& couldCompound = compIt->second;
                    vector<MatchExpression*> toCompound;
                    vector<MatchExpression*> assigned = indexAssign.preds;
                    for (const auto& orPushdown : indexAssign.orPushdowns) {
                        assigned.push_back(orPushdown.first);
                    }

                    // ...select the predicates that are safe to compound and compound them.
                    getMultikeyCompoundablePreds(assigned, couldCompound, &toCompound);

                    for (auto pred : toCompound) {
                        assignPredicate(
                            outsidePreds, pred, getPosition(thisIndex, pred), &indexAssign);
                    }
                }

                // Do not output this assignment if it consists only of outside predicates.
                if (!indexAssign.preds.empty()) {
                    AndEnumerableState state;
                    state.assignments.push_back(std::move(indexAssign));
                    andAssignment->choices.push_back(std::move(state));
                }
            }
        } else {
            // The assignment we're filling out.
            OneIndexAssignment indexAssign;

            // This is the index we assign to.
            indexAssign.index = it->first;

            // The index isn't multikey.  Assign all preds to it.  The planner will
            // intersect the bounds.
            for (auto pred : it->second) {
                assignPredicate(outsidePreds, pred, 0, &indexAssign);
            }

            // Find everything that could use assign.index but isn't a pred over
            // the first field of that index.
            IndexToPredMap::const_iterator compIt = idxToNotFirst.find(indexAssign.index);
            if (compIt != idxToNotFirst.end()) {
                for (auto pred : compIt->second) {
                    assignPredicate(outsidePreds, pred, getPosition(thisIndex, pred), &indexAssign);
                }
            }

            // Output the assignment.
            invariant(!indexAssign.preds.empty());
            AndEnumerableState state;
            state.assignments.push_back(std::move(indexAssign));
            andAssignment->choices.push_back(std::move(state));
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

        // We create a scan per predicate so if we have >1 predicate we'll already
        // have at least 2 scans (one predicate per scan as the planner can't
        // intersect bounds when the index is multikey), so we stop here.
        if (oneIndex.multikey && firstIt->second.size() > 1) {
            OneIndexAssignment oneAssign;
            oneAssign.index = firstIt->first;
            oneAssign.preds = firstIt->second;
            // Since everything in assign.preds prefixes the index, they all go at position '0' in
            // the index, the first position.
            oneAssign.positions.resize(oneAssign.preds.size(), 0);

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
            state.assignments.push_back(std::move(oneAssign));
            andAssignment->choices.push_back(std::move(state));
            continue;
        }

        // Output (subnode, firstAssign) pairs.
        for (size_t i = 0; i < subnodes.size(); ++i) {
            OneIndexAssignment oneAssign;
            oneAssign.index = firstIt->first;
            oneAssign.preds = firstIt->second;
            // Since everything in assign.preds prefixes the index, they all go at position '0' in
            // the index, the first position.
            oneAssign.positions.resize(oneAssign.preds.size(), 0);

            AndEnumerableState indexAndSubnode;
            indexAndSubnode.assignments.push_back(std::move(oneAssign));
            indexAndSubnode.subnodesToIndex.push_back(subnodes[i]);
            andAssignment->choices.push_back(std::move(indexAndSubnode));
            // Limit n^2.
            if (andAssignment->choices.size() - sizeBefore > _intersectLimit) {

                LOGV2_DEBUG(4862502,
                            1,
                            "plan enumerator exceeded threshold for AND enumerations",
                            "intersectLimit"_attr = _intersectLimit);
                _explainInfo.hitIndexedAndLimit = true;
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
                LOGV2_DEBUG(4862503,
                            1,
                            "plan enumerator exceeded threshold for AND enumerations",
                            "intersectLimit"_attr = _intersectLimit);
                _explainInfo.hitIndexedAndLimit = true;
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
            state.assignments.push_back(std::move(firstAssign));
            state.assignments.push_back(std::move(secondAssign));
            andAssignment->choices.push_back(std::move(state));
        }
    }
}

void PlanEnumerator::getIndexedPreds(MatchExpression* node,
                                     PrepMemoContext context,
                                     std::vector<MatchExpression*>* indexedPreds) {
    if (Indexability::nodeCanUseIndexOnOwnField(node)) {
        RelevantTag* rt = static_cast<RelevantTag*>(node->getTag());
        if (context.elemMatchExpr) {
            // If we're in an $elemMatch context, store the
            // innermost parent $elemMatch, as well as the
            // inner path prefix.
            rt->elemMatchExpr = context.elemMatchExpr;
            rt->pathPrefix = getPathPrefix(node->path().toString());
        } else {
            // We're not an $elemMatch context, so we should store
            // the prefix of the full path.
            rt->pathPrefix = getPathPrefix(rt->path);
        }

        // Output this as a pred that can use the index.
        indexedPreds->push_back(node);
    } else if (Indexability::isBoundsGeneratingNot(node)) {
        getIndexedPreds(node->getChild(0), context, indexedPreds);
    } else if (MatchExpression::ELEM_MATCH_OBJECT == node->matchType()) {
        PrepMemoContext childContext;
        childContext.elemMatchExpr = node;
        for (size_t i = 0; i < node->numChildren(); ++i) {
            getIndexedPreds(node->getChild(i), childContext, indexedPreds);
        }
    } else if (MatchExpression::AND == node->matchType()) {
        for (size_t i = 0; i < node->numChildren(); ++i) {
            getIndexedPreds(node->getChild(i), context, indexedPreds);
        }
    }
}

bool PlanEnumerator::prepSubNodes(MatchExpression* node,

                                  PrepMemoContext context,
                                  vector<MemoID>* subnodesOut,
                                  vector<MemoID>* mandatorySubnodes) {
    for (size_t i = 0; i < node->numChildren(); ++i) {
        MatchExpression* child = node->getChild(i);
        if (MatchExpression::OR == child->matchType()) {
            if (_orLimit == 0) {
                LOGV2_DEBUG(4862500,
                            1,
                            "plan enumerator exceeded threshold for OR enumerations",
                            "orEnumerationLimit"_attr = _orLimit);
                _explainInfo.hitIndexedOrLimit = true;
                return false;
            }
            bool mandatory = expressionRequiresIndex(child);
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
        } else if (MatchExpression::ELEM_MATCH_OBJECT == child->matchType()) {
            PrepMemoContext childContext;
            childContext.elemMatchExpr = child;
            childContext.outsidePreds = context.outsidePreds;
            markTraversedThroughElemMatchObj(&childContext);
            prepSubNodes(child, childContext, subnodesOut, mandatorySubnodes);
        } else if (MatchExpression::AND == child->matchType()) {
            prepSubNodes(child, context, subnodesOut, mandatorySubnodes);
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
    stdx::unordered_map<MatchExpression*, set<string>> used;

    // Initialize 'used' with the starting predicates in 'assigned'. Begin by
    // initializing the top-level scope with the prefix of the full path.
    for (size_t i = 0; i < assigned.size(); i++) {
        const MatchExpression* assignedPred = assigned[i];
        invariant(nullptr != assignedPred->getTag());
        RelevantTag* usedRt = static_cast<RelevantTag*>(assignedPred->getTag());
        set<string> usedPrefixes;
        usedPrefixes.insert(getPathPrefix(usedRt->path));
        used[nullptr] = usedPrefixes;

        // If 'assigned' is a predicate inside an $elemMatch, we have to
        // add the prefix not only to the top-level context, but also to the
        // the $elemMatch context. For example, if 'assigned' is {a: {$elemMatch: {b: 1}}},
        // then we will have already added "a" to the set for NULL. We now
        // also need to add "b" to the set for the $elemMatch.
        if (nullptr != usedRt->elemMatchExpr) {
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
            invariant(used.end() != used.find(nullptr));
            set<string>& topLevelUsed = used.find(nullptr)->second;

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

void PlanEnumerator::assignMultikeySafePredicates(
    const std::vector<MatchExpression*>& couldAssign,
    const stdx::unordered_map<MatchExpression*, OutsidePredRoute>& outsidePreds,
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

    // Update 'used' with all outside predicates already assigned to 'thisIndex';
    for (const auto& orPushdown : indexAssignment->orPushdowns) {
        invariant(orPushdown.first->getTag());
        RelevantTag* rt = static_cast<RelevantTag*>(orPushdown.first->getTag());

        // Any outside predicates already assigned to 'thisIndex' were assigned in the first
        // position.
        const size_t position = 0;
        const bool shouldHaveAssigned =
            canAssignPredToIndex(rt, thisIndex.multikeyPaths[position], &used);
        invariant(shouldHaveAssigned);
    }

    size_t posInIdx = 0;

    for (const auto& keyElem : thisIndex.keyPattern) {
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
                assignPredicate(outsidePreds, couldAssignPred, posInIdx, indexAssignment);
                continue;
            }

            // See if any of the predicates that are already assigned to 'thisIndex' prevent us from
            // assigning 'couldAssignPred' as well.
            const bool shouldAssign =
                canAssignPredToIndex(rt, thisIndex.multikeyPaths[posInIdx], &used);

            if (shouldAssign) {
                assignPredicate(outsidePreds, couldAssignPred, posInIdx, indexAssignment);
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

size_t PlanEnumerator::getPosition(const IndexEntry& indexEntry, MatchExpression* predicate) {
    invariant(predicate->getTag());
    RelevantTag* relevantTag = static_cast<RelevantTag*>(predicate->getTag());
    size_t position = 0;
    for (auto&& element : indexEntry.keyPattern) {
        if (element.fieldName() == relevantTag->path) {
            return position;
        }
        ++position;
    }
    MONGO_UNREACHABLE;
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
    LOGV2_DEBUG(20944, 5, "Tagging memoID", "id"_attr = id);
    NodeAssignment* assign = _memo[id];
    verify(nullptr != assign);

    if (nullptr != assign->orAssignment) {
        OrAssignment* oa = assign->orAssignment.get();
        for (size_t i = 0; i < oa->subnodes.size(); ++i) {
            tagMemo(oa->subnodes[i]);
        }
    } else if (nullptr != assign->lockstepOrAssignment) {
        LockstepOrAssignment* oa = assign->lockstepOrAssignment.get();
        for (auto&& node : oa->subnodes) {
            tagMemo(node.memoId);
        }
    } else if (nullptr != assign->arrayAssignment) {
        ArrayAssignment* aa = assign->arrayAssignment.get();
        tagMemo(aa->subnodes[aa->counter]);
    } else if (nullptr != assign->andAssignment) {
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
                if (pred->getTag()) {
                    OrPushdownTag* orPushdownTag = static_cast<OrPushdownTag*>(pred->getTag());
                    orPushdownTag->setIndexTag(
                        new IndexTag(assign.index, assign.positions[j], assign.canCombineBounds));
                } else {
                    pred->setTag(
                        new IndexTag(assign.index, assign.positions[j], assign.canCombineBounds));
                }
            }

            // Add all OrPushdownTags for this index assignment.
            for (const auto& orPushdown : assign.orPushdowns) {
                auto expr = orPushdown.first;
                if (!expr->getTag()) {
                    expr->setTag(new OrPushdownTag());
                }
                OrPushdownTag* orPushdownTag = static_cast<OrPushdownTag*>(expr->getTag());
                orPushdownTag->addDestination(orPushdown.second.clone());
            }
        }
    } else {
        verify(0);
    }
}

bool PlanEnumerator::LockstepOrAssignment::allIdentical() const {
    const auto firstCounter = subnodes[0].iterationCount;
    for (auto&& subnode : subnodes) {
        if (subnode.iterationCount != firstCounter) {
            return false;
        }
    }
    return true;
}

bool PlanEnumerator::LockstepOrAssignment::shouldResetBeforeProceeding(
    size_t totalEnumerated) const {
    if (totalEnumerated == 0 || !exhaustedLockstepIteration) {
        return false;
    }

    size_t totalPossibleEnumerations = 1;
    for (auto&& subnode : subnodes) {
        if (!subnode.maxIterCount) {
            return false;  // Haven't yet looped over this child entirely, not ready yet.
        }
        totalPossibleEnumerations *= subnode.maxIterCount.value();
    }

    // If we're able to compute a total number expected enumerations, we must have already cycled
    // through each of the subnodes at least once. So if we've done that and then iterated all
    // possible enumerations, we're about to repeat ourselves.
    return totalEnumerated % totalPossibleEnumerations == 0;
}

bool PlanEnumerator::_nextMemoForLockstepOrAssignment(
    PlanEnumerator::LockstepOrAssignment* assignment) {

    if (!assignment->exhaustedLockstepIteration) {
        // We have not yet finished advancing all children simultaneously, so we'll loop over
        // each child and advance it.

        // Because we're doing things in a special order, we have to be careful to not duplicate
        // ourselves. If each child has the same number of alternatives, we will eventually
        // "carry" or roll over each child back to the beginning. When this happens, we should
        // not return that plan again.
        bool everyoneRolledOver = true;

        for (auto&& node : assignment->subnodes) {
            ++node.iterationCount;
            const bool wrappedAround = nextMemo(node.memoId);
            if (wrappedAround) {
                node.maxIterCount = node.iterationCount;
                node.iterationCount = 0;
                // We ran out of "lockstep runway" of sorts. At least one of the subnodes was
                // exhausted, so this will be our last time advancing all children in lockstep.
                assignment->exhaustedLockstepIteration = true;
            } else {
                everyoneRolledOver = false;
            }
        }
        // Edge case: if every child has only one option available, we are already finished
        // enumerating.
        if (assignment->shouldResetBeforeProceeding(assignment->totalEnumerated)) {
            assignment->exhaustedLockstepIteration = false;
            return true;  // We're back at the beginning, no need to reset.
        }
        if (!everyoneRolledOver) {
            // Either there's more lockstep iteration to come, or the subnodes have different
            // amounts of options. In either case, we are now in a new enumeration state so just
            // return.
            return false;
        }
        // Otherwise we just rolled over and went back to the first enumeration state, so we need to
        // keep going to avoid duplicating that state. Fall through to below to start "normal", not
        // lockstep iteration.
    }

    auto advanceOnce = [this, assignment]() {
        for (auto&& node : assignment->subnodes) {
            ++node.iterationCount;
            const bool wrappedAround = nextMemo(node.memoId);
            if (!wrappedAround) {
                return;
            }
            node.maxIterCount = node.iterationCount;
            node.iterationCount = 0;
        }
    };
    advanceOnce();
    while (assignment->allIdentical()) {
        // All sub-nodes have the same enumeration state, skip this one since we already did
        // it above. This is expected to happen pretty often. For example, if we have two subnodes
        // each enumerating two states, we'd expect the order to be: 00, 11 (these two iterated
        // above), then 00 (skipped by falling through above after finishing lockstep iteration),
        // then 10, 11 (skipped here), 00 (skipped here), then finally 01.
        advanceOnce();
    }

    // This special ordering is tricky to reset. Because it iterates the sub nodes in such a
    // unique order, it can be difficult to know when it has actually finished iterating. Our
    // strategy is just to compute a total and go back to the beginning once we hit that total.
    if (!assignment->shouldResetBeforeProceeding(assignment->totalEnumerated)) {
        return false;
    }
    // Reset!
    for (auto&& subnode : assignment->subnodes) {
        while (!nextMemo(subnode.memoId)) {
            // Keep advancing till it rolls over.
        }
        subnode.iterationCount = 0;
    }
    assignment->exhaustedLockstepIteration = false;
    return true;
}

bool PlanEnumerator::nextMemo(size_t id) {
    NodeAssignment* assign = _memo[id];
    verify(nullptr != assign);

    if (nullptr != assign->orAssignment) {
        OrAssignment* oa = assign->orAssignment.get();

        // Limit the number of OR enumerations.
        oa->counter++;
        if (oa->counter >= _orLimit) {
            LOGV2_DEBUG(3639300,
                        1,
                        "plan enumerator exceeded threshold for OR enumerations",
                        "orEnumerationLimit"_attr = _orLimit);
            _explainInfo.hitIndexedOrLimit = true;
            return true;
        }

        // OR just walks through telling its children to move forward.
        for (size_t i = 0; i < oa->subnodes.size(); ++i) {
            // If there's no carry, we just stop. If there's a carry, we move the next child
            // forward.
            if (!nextMemo(oa->subnodes[i])) {
                return false;
            }
        }
        // If we're here, the last subnode had a carry, therefore the OR has a carry.
        return true;
    } else if (nullptr != assign->lockstepOrAssignment) {
        LockstepOrAssignment* assignment = assign->lockstepOrAssignment.get();

        // Limit the number of OR enumerations.
        ++assignment->totalEnumerated;
        if (assignment->totalEnumerated >= _orLimit) {
            LOGV2_DEBUG(3639301,
                        1,
                        "plan enumerator exceeded threshold for OR enumerations",
                        "orEnumerationLimit"_attr = _orLimit);
            _explainInfo.hitIndexedOrLimit = true;
            return true;
        }
        return _nextMemoForLockstepOrAssignment(assignment);
    } else if (nullptr != assign->arrayAssignment) {
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
    } else if (nullptr != assign->andAssignment) {
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
    } else {
        MONGO_UNREACHABLE;
    }
}

}  // namespace mongo

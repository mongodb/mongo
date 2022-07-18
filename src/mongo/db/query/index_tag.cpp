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

#include "mongo/db/query/index_tag.h"

#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/indexability.h"
#include "mongo/stdx/unordered_map.h"

#include <algorithm>
#include <limits>

namespace mongo {

using TagType = MatchExpression::TagData::Type;

namespace {

// Compares 'lhs' for 'rhs', using the tag-based ordering expected by the access planner. Returns a
// negative number if 'lhs' is smaller than 'rhs', 0 if they are equal, and 1 if 'lhs' is larger.
int tagComparison(const MatchExpression* lhs, const MatchExpression* rhs) {
    IndexTag* lhsTag = dynamic_cast<IndexTag*>(lhs->getTag());
    size_t lhsValue = lhsTag ? lhsTag->index : IndexTag::kNoIndex;
    size_t lhsPos = lhsTag ? lhsTag->pos : IndexTag::kNoIndex;

    IndexTag* rhsTag = dynamic_cast<IndexTag*>(rhs->getTag());
    size_t rhsValue = rhsTag ? rhsTag->index : IndexTag::kNoIndex;
    size_t rhsPos = rhsTag ? rhsTag->pos : IndexTag::kNoIndex;

    // First, order on indices.
    if (lhsValue != rhsValue) {
        // This relies on kNoIndex being larger than every other possible index.
        return lhsValue < rhsValue ? -1 : 1;
    }

    // Next, order so that if there's a GEO_NEAR it's first.
    if (MatchExpression::GEO_NEAR == lhs->matchType()) {
        return -1;
    } else if (MatchExpression::GEO_NEAR == rhs->matchType()) {
        return 1;
    }

    // Ditto text.
    if (MatchExpression::TEXT == lhs->matchType()) {
        return -1;
    } else if (MatchExpression::TEXT == rhs->matchType()) {
        return 1;
    }

    // Next, order so that the first field of a compound index appears first.
    if (lhsPos != rhsPos) {
        return lhsPos < rhsPos ? -1 : 1;
    }

    // Next, order on fields.
    int cmp = lhs->path().compare(rhs->path());
    if (0 != cmp) {
        return cmp;
    }

    // Next, order on expression type.
    if (lhs->matchType() != rhs->matchType()) {
        return lhs->matchType() < rhs->matchType() ? -1 : 1;
    }

    // The 'lhs' and 'rhs' are equal. Break ties by comparing child nodes.
    const size_t numChildren = std::min(lhs->numChildren(), rhs->numChildren());
    for (size_t childIdx = 0; childIdx < numChildren; ++childIdx) {
        int childCompare = tagComparison(lhs->getChild(childIdx), rhs->getChild(childIdx));
        if (childCompare != 0) {
            return childCompare;
        }
    }

    // If all else is equal, sort whichever node has fewer children first.
    if (lhs->numChildren() != rhs->numChildren()) {
        return lhs->numChildren() < rhs->numChildren() ? -1 : 1;
    }

    return 0;
}

// Sorts the tree using its IndexTag(s). Nodes that use the same index will sort so that they are
// adjacent to one another.
void sortUsingTags(MatchExpression* tree) {
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        sortUsingTags(tree->getChild(i));
    }

    if (auto&& children = tree->getChildVector())
        std::stable_sort(children->begin(), children->end(), [](auto&& lhs, auto&& rhs) {
            return tagComparison(lhs.get(), rhs.get()) < 0;
        });
}

// Attaches 'node' to 'target'. If 'target' is an AND, adds 'node' as a child of 'target'.
// Otherwise, creates an AND that is a child of 'targetParent' at position 'targetPosition', and
// adds 'target' and 'node' as its children. Tags 'node' with 'tagData'.
void attachNode(MatchExpression* node,
                MatchExpression* target,
                OrMatchExpression* targetParent,
                size_t targetPosition,
                std::unique_ptr<MatchExpression::TagData> tagData) {
    auto clone = node->shallowClone();
    if (clone->matchType() == MatchExpression::NOT) {
        IndexTag* indexTag = static_cast<IndexTag*>(tagData.get());
        clone->setTag(new IndexTag(indexTag->index));
        clone->getChild(0)->setTag(tagData.release());
    } else {
        clone->setTag(tagData.release());
    }

    if (MatchExpression::AND == target->matchType()) {
        auto andNode = static_cast<AndMatchExpression*>(target);
        andNode->add(std::move(clone));
    } else {
        auto andNode = std::make_unique<AndMatchExpression>();
        auto indexTag = static_cast<IndexTag*>(clone->getTag());
        andNode->setTag(new IndexTag(indexTag->index));
        andNode->add(std::move((*targetParent->getChildVector())[targetPosition]));
        andNode->add(std::move(clone));
        targetParent->getChildVector()->operator[](targetPosition) = std::move(andNode);
    }
}

// Partitions destinations according to the first element of the destination's route. Trims the
// first element off of each destination's route.
stdx::unordered_map<size_t, std::vector<OrPushdownTag::Destination>> partitionChildDestinations(
    std::vector<OrPushdownTag::Destination> destinations) {
    stdx::unordered_map<size_t, std::vector<OrPushdownTag::Destination>> childDestinations;
    for (auto&& dest : destinations) {
        invariant(!dest.route.empty());
        auto index = dest.route.front();
        dest.route.pop_front();
        childDestinations[index].push_back(std::move(dest));
    }
    return childDestinations;
}

// Finds the node within 'tree' that is an indexed OR, if one exists.
MatchExpression* getIndexedOr(MatchExpression* tree) {
    if (MatchExpression::OR == tree->matchType() && tree->getTag()) {
        return tree;
    }
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        if (auto indexedOrChild = getIndexedOr(tree->getChild(i))) {
            return indexedOrChild;
        }
    }
    return nullptr;
}

// Pushes down 'node' along the routes in 'target' specified in 'destinations'. Each value in the
// route is the index of a child in an indexed OR. Returns true if 'node' is moved to every indexed
// descendant of 'target'.
bool pushdownNode(MatchExpression* node,
                  MatchExpression* target,
                  std::vector<OrPushdownTag::Destination> destinations) {
    if (MatchExpression::OR == target->matchType()) {
        OrMatchExpression* orNode = static_cast<OrMatchExpression*>(target);
        bool moveToAllChildren = true;
        auto childDestinationsMap = partitionChildDestinations(std::move(destinations));
        for (size_t i = 0; i < orNode->numChildren(); ++i) {
            auto childDestinations = childDestinationsMap.find(i);
            if (childDestinations == childDestinationsMap.end()) {

                // This child was not specified by any route in 'destinations'.
                moveToAllChildren = false;
            } else {
                invariant(!childDestinations->second.empty());
                if (childDestinations->second[0].route.empty()) {

                    // There should only be one destination if we have reached the end of a route.
                    // Otherwise, we started with duplicate routes.
                    invariant(childDestinations->second.size() == 1);

                    // We have reached the position at which to attach 'node'.
                    attachNode(node,
                               orNode->getChild(i),
                               orNode,
                               i,
                               std::move(childDestinations->second[0].tagData));
                } else {

                    // This child was specified by a non-trivial route in destinations, so we recur.
                    moveToAllChildren = pushdownNode(node,
                                                     orNode->getChild(i),
                                                     std::move(childDestinations->second)) &&
                        moveToAllChildren;
                }
            }
        }
        return moveToAllChildren;
    }

    if (MatchExpression::AND == target->matchType()) {
        auto indexedOr = getIndexedOr(target);
        invariant(indexedOr);
        return pushdownNode(node, indexedOr, std::move(destinations));
    }

    MONGO_UNREACHABLE_TASSERT(4457014);
}

// Populates 'out' with all descendants of 'node' that have OrPushdownTags, assuming the initial
// input is an ELEM_MATCH_OBJECT.
void getElemMatchOrPushdownDescendants(MatchExpression* node, std::vector<MatchExpression*>* out) {
    if (node->getTag() && node->getTag()->getType() == TagType::OrPushdownTag) {
        out->push_back(node);
    } else if (node->matchType() == MatchExpression::ELEM_MATCH_OBJECT ||
               node->matchType() == MatchExpression::AND) {
        for (size_t i = 0; i < node->numChildren(); ++i) {
            getElemMatchOrPushdownDescendants(node->getChild(i), out);
        }
    } else if (node->matchType() == MatchExpression::NOT) {
        // The immediate child of NOT may be tagged, but there should be no tags deeper than this.
        auto* childNode = node->getChild(0);
        if (childNode->getTag() && childNode->getTag()->getType() == TagType::OrPushdownTag) {
            out->push_back(node);
        }
    }
}

// Attempts to push the given node down into the 'indexedOr' subtree. Returns true if the predicate
// can subsequently be trimmed from the MatchExpression tree, false otherwise.
bool processOrPushdownNode(MatchExpression* node, MatchExpression* indexedOr) {
    // If the node is a negation, then its child is the predicate node that may be tagged.
    auto* predNode = node->matchType() == MatchExpression::NOT ? node->getChild(0) : node;

    // If the predicate node is not tagged for pushdown, return false immediately.
    if (!predNode->getTag() || predNode->getTag()->getType() != TagType::OrPushdownTag) {
        return false;
    }
    invariant(indexedOr);

    // Predicate node is tagged for pushdown. Extract its route through the $or and its index tag.
    auto* orPushdownTag = static_cast<OrPushdownTag*>(predNode->getTag());
    auto destinations = orPushdownTag->releaseDestinations();
    auto indexTag = orPushdownTag->releaseIndexTag();
    predNode->setTag(nullptr);

    // Attempt to push the node into the indexedOr, then re-set its tag to the indexTag.
    const bool pushedDown = pushdownNode(node, indexedOr, std::move(destinations));
    predNode->setTag(indexTag.release());

    // Return true if we can trim the predicate. We could trim the node even if it had an index tag
    // for this position, but that could make the index tagging of the tree wrong.
    return pushedDown && !predNode->getTag();
}

// Finds all the nodes in the tree with OrPushdownTags and copies them to the Destinations specified
// in the OrPushdownTag, tagging them with the TagData in the Destination. Removes the node from its
// current location if possible.
void resolveOrPushdowns(MatchExpression* tree) {
    if (tree->numChildren() == 0) {
        return;
    }
    if (MatchExpression::AND == tree->matchType()) {
        AndMatchExpression* andNode = static_cast<AndMatchExpression*>(tree);
        MatchExpression* indexedOr = getIndexedOr(andNode);

        if (indexedOr) {
            for (size_t i = 0; i < andNode->numChildren(); ++i) {
                auto child = andNode->getChild(i);

                // For ELEM_MATCH_OBJECT, we push down all tagged descendants. However, we cannot
                // trim any of these predicates, since the $elemMatch filter must be applied in its
                // entirety.
                if (child->matchType() == MatchExpression::ELEM_MATCH_OBJECT) {
                    std::vector<MatchExpression*> orPushdownDescendants;
                    getElemMatchOrPushdownDescendants(child, &orPushdownDescendants);
                    for (auto descendant : orPushdownDescendants) {
                        static_cast<void>(processOrPushdownNode(descendant, indexedOr));
                    }
                } else if (processOrPushdownNode(child, indexedOr)) {
                    // The indexed $or can completely satisfy the child predicate, so we trim it.
                    auto ownedChild = andNode->removeChild(i);
                    --i;
                }
            }
        }
    }
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        resolveOrPushdowns(tree->getChild(i));
    }
}

}  // namespace

const size_t IndexTag::kNoIndex = std::numeric_limits<size_t>::max();

void prepareForAccessPlanning(MatchExpression* tree) {
    resolveOrPushdowns(tree);
    sortUsingTags(tree);
}

}  // namespace mongo

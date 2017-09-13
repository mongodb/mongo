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

#include "mongo/db/query/index_tag.h"

#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/indexability.h"
#include "mongo/platform/unordered_map.h"

#include <algorithm>
#include <limits>

namespace mongo {

using TagType = MatchExpression::TagData::Type;

namespace {

bool TagComparison(const MatchExpression* lhs, const MatchExpression* rhs) {
    IndexTag* lhsTag = static_cast<IndexTag*>(lhs->getTag());
    size_t lhsValue = (NULL == lhsTag) ? IndexTag::kNoIndex : lhsTag->index;
    size_t lhsPos = (NULL == lhsTag) ? IndexTag::kNoIndex : lhsTag->pos;

    IndexTag* rhsTag = static_cast<IndexTag*>(rhs->getTag());
    size_t rhsValue = (NULL == rhsTag) ? IndexTag::kNoIndex : rhsTag->index;
    size_t rhsPos = (NULL == rhsTag) ? IndexTag::kNoIndex : rhsTag->pos;

    // First, order on indices.
    if (lhsValue != rhsValue) {
        // This relies on kNoIndex being larger than every other possible index.
        return lhsValue < rhsValue;
    }

    // Next, order so that if there's a GEO_NEAR it's first.
    if (MatchExpression::GEO_NEAR == lhs->matchType()) {
        return true;
    } else if (MatchExpression::GEO_NEAR == rhs->matchType()) {
        return false;
    }

    // Ditto text.
    if (MatchExpression::TEXT == lhs->matchType()) {
        return true;
    } else if (MatchExpression::TEXT == rhs->matchType()) {
        return false;
    }

    // Next, order so that the first field of a compound index appears first.
    if (lhsPos != rhsPos) {
        return lhsPos < rhsPos;
    }

    // Next, order on fields.
    int cmp = lhs->path().compare(rhs->path());
    if (0 != cmp) {
        return 0;
    }

    // Finally, order on expression type.
    return lhs->matchType() < rhs->matchType();
}

// Sorts the tree using its IndexTag(s). Nodes that use the same index will sort so that they are
// adjacent to one another.
void sortUsingTags(MatchExpression* tree) {
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        sortUsingTags(tree->getChild(i));
    }
    std::vector<MatchExpression*>* children = tree->getChildVector();
    if (NULL != children) {
        std::sort(children->begin(), children->end(), TagComparison);
    }
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
        AndMatchExpression* andNode = static_cast<AndMatchExpression*>(target);
        andNode->add(clone.release());
    } else {
        std::unique_ptr<AndMatchExpression> andNode = stdx::make_unique<AndMatchExpression>();
        IndexTag* indexTag = static_cast<IndexTag*>(clone->getTag());
        andNode->setTag(new IndexTag(indexTag->index));
        andNode->add(target);
        andNode->add(clone.release());
        targetParent->getChildVector()->operator[](targetPosition) = andNode.release();
    }
}

// Partitions destinations according to the first element of the destination's route. Trims the
// first element off of each destination's route.
unordered_map<size_t, std::vector<OrPushdownTag::Destination>> partitionChildDestinations(
    std::vector<OrPushdownTag::Destination> destinations) {
    unordered_map<size_t, std::vector<OrPushdownTag::Destination>> childDestinations;
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

    MONGO_UNREACHABLE;
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
    }
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

        for (size_t i = 0; i < andNode->numChildren(); ++i) {
            auto child = andNode->getChild(i);
            if (child->getTag() && child->getTag()->getType() == TagType::OrPushdownTag) {
                invariant(indexedOr);
                OrPushdownTag* orPushdownTag = static_cast<OrPushdownTag*>(child->getTag());
                auto destinations = orPushdownTag->releaseDestinations();
                auto indexTag = orPushdownTag->releaseIndexTag();
                child->setTag(nullptr);
                if (pushdownNode(child, indexedOr, std::move(destinations)) && !indexTag) {

                    // indexedOr can completely satisfy the predicate specified in child, so we can
                    // trim it. We could remove the child even if it had an index tag for this
                    // position, but that could make the index tagging of the tree wrong.
                    auto ownedChild = andNode->removeChild(i);

                    // We removed child i, so decrement the child index.
                    --i;
                } else {
                    child->setTag(indexTag.release());
                }
            } else if (child->matchType() == MatchExpression::NOT && child->getChild(0)->getTag() &&
                       child->getChild(0)->getTag()->getType() == TagType::OrPushdownTag) {
                invariant(indexedOr);
                OrPushdownTag* orPushdownTag =
                    static_cast<OrPushdownTag*>(child->getChild(0)->getTag());
                auto destinations = orPushdownTag->releaseDestinations();
                auto indexTag = orPushdownTag->releaseIndexTag();
                child->getChild(0)->setTag(nullptr);

                // Push down the NOT and its child.
                if (pushdownNode(child, indexedOr, std::move(destinations)) && !indexTag) {

                    // indexedOr can completely satisfy the predicate specified in child, so we can
                    // trim it. We could remove the child even if it had an index tag for this
                    // position, but that could make the index tagging of the tree wrong.
                    auto ownedChild = andNode->removeChild(i);

                    // We removed child i, so decrement the child index.
                    --i;
                } else {
                    child->getChild(0)->setTag(indexTag.release());
                }
            } else if (child->matchType() == MatchExpression::ELEM_MATCH_OBJECT) {

                // Push down all descendants of child with OrPushdownTags.
                std::vector<MatchExpression*> orPushdownDescendants;
                getElemMatchOrPushdownDescendants(child, &orPushdownDescendants);
                if (!orPushdownDescendants.empty()) {
                    invariant(indexedOr);
                }
                for (auto descendant : orPushdownDescendants) {
                    OrPushdownTag* orPushdownTag =
                        static_cast<OrPushdownTag*>(descendant->getTag());
                    auto destinations = orPushdownTag->releaseDestinations();
                    auto indexTag = orPushdownTag->releaseIndexTag();
                    descendant->setTag(nullptr);
                    pushdownNode(descendant, indexedOr, std::move(destinations));
                    descendant->setTag(indexTag.release());

                    // We cannot trim descendants of an $elemMatch object, since the filter must
                    // be applied in its entirety.
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

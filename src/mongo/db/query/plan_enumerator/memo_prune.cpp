/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/db/query/plan_enumerator/memo_prune.h"

#include <algorithm>
#include <set>

#include <absl/hash/hash.h>

#include "mongo/bson/bsonelement.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/path.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/map_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace plan_enumerator {
namespace {

// Don't prune AndEnumerableStates doing intersection or that have any subnodes to index. This
// pruning is intentionally safe to make sure we don't lose the best plan.
bool isPruneableAndState(const AndEnumerableState& andState) {
    return andState.assignments.size() == 1 && andState.subnodesToIndex.empty();
}

typedef std::pair<OneIndexAssignment, size_t> AssignmentAndPos;

struct AssignmentLt {
    bool operator()(const AssignmentAndPos& assignment1,
                    const AssignmentAndPos& assignment2) const {
        absl::Hash<OneIndexAssignment> hasher;
        return hasher(assignment1.first) < hasher(assignment2.first);
    }
};

enum IndexCmpResult { kNotInterchangeable, kInterchangeableShorter, kInterchangeableLonger };

/*
 * Determines if the two OneIndexAssignment are interchangeable for this query. This means that one
 * does not provide any additional information over the other in the context of this query. We will
 * deduplicate preferring the index with the shorter key pattern, returning kInterchangeableShorter
 * if a1 is interchangeable with a2 but shorter, or kInterchangeableLonger if it's interchangeable
 * but longer. Otherwise return kNotInterchangeable. The purpose of this deduplication is to
 * multiplan less redundant plans, or if we deduplicate down to just one index we don't have to
 * multiplan at all.
 */
IndexCmpResult cmpIndexAssignments(const OneIndexAssignment& a1,
                                   const OneIndexAssignment& a2,
                                   const QueryPruningInfo& queryInfo) {
    // TODO SERVER-86639 analyze shard key to make sure we don't dedup useful indexes involving the
    // shard key. Also consider making this logic less strict in other ways, for example the sort
    // pattern and covering index logic can do more analysis. And maybe we don't have to bail if the
    // index types are non-btree but are the same type.
    if (!queryInfo.shardKey.isEmpty()) {
        return kNotInterchangeable;
    }
    // If we generated two assignments that use the same index, it's because we're using the index
    // in two different ways. There are multiple ways to create index bounds from some predicates.
    // We don't want to dedup in these situations.
    if (a1.index == a2.index) {
        return kNotInterchangeable;
    }

    const auto& index1 = (*queryInfo.indices)[a1.index];
    const auto& index2 = (*queryInfo.indices)[a2.index];
    // Only dedup indexes when they are both type btree. Also bail on any special indexes such as
    // unique, sparse, collated, or partial. This check could possibly be less strict.
    if (index1.type != INDEX_BTREE || index2.type != INDEX_BTREE || index1.unique ||
        index2.unique || index1.sparse || index2.sparse || index1.collator || index2.collator ||
        index1.filterExpr || index2.filterExpr) {
        return kNotInterchangeable;
    }

    // Now we check that the same predicates are used in the scan, in the same positions.
    if (a1.preds.size() != a2.preds.size()) {
        return kNotInterchangeable;
    }
    // `lastPosUsed` is the the last position of the key pattern used for a predicate. If we have
    // the same key pattern from 0 to lastPos (inclusive both ends), the indexes are
    // interchangeable. An example might be indexes {a: 1, b: 1} and {a: 1, b: 1, c: 1} where the
    // query asks for documents with a=1 and b=1. These indexes provide the same coverage of
    // predicates, except the a,b,c index is less favorable due to having more data and entries.
    IndexPosition lastPosUsed = 0;
    for (size_t i = 0; i < a1.preds.size(); i++) {
        // If they are dups, the assignments should be using the same predicates and positions in
        // the index.
        if (a1.preds[i] != a2.preds[i] || a1.positions[i] != a2.positions[i]) {
            return kNotInterchangeable;
        }
        lastPosUsed = std::max(lastPosUsed, a1.positions[i]);
    }

    // To be interchangeable the indexes must have the same key pattern up until the last position
    // used by the predicates.
    BSONObjIterator patternIt1(index1.keyPattern);
    BSONObjIterator patternIt2(index2.keyPattern);
    BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kConsider, nullptr);
    for (size_t i = 0; i <= lastPosUsed; i++) {
        auto key1 = patternIt1.next();
        auto key2 = patternIt2.next();
        if (eltCmp.evaluate(key1 != key2)) {
            return kNotInterchangeable;
        }
    }

    // Check the rest of the key patterns in case we might lose a sort via index or a covering
    // index.
    while (patternIt1.more() || patternIt2.more()) {
        auto keyFieldName = (patternIt1.more() ? patternIt1 : patternIt2).next().fieldName();
        // We want to be able to use {a: 1, b: 1} instead of {a: 1} if we have query a=1 and only
        // want fields 'a' and 'b' (cover with an index). So if there's a projection that doesn't
        // require the entire doc, and the indexes have a required field in the key pattern, let's
        // bail.
        if (queryInfo.projection && !queryInfo.projection->requiresDocument() &&
            queryInfo.projection->getRequiredFields().contains(keyFieldName)) {
            return kNotInterchangeable;
        }
        // Same idea as above but for sort. We don't want to miss out on covering a sort with an
        // index.
        if (queryInfo.sortPatFields && queryInfo.sortPatFields->contains(keyFieldName)) {
            return kNotInterchangeable;
        }
    }

    // If we made it here they are dups. We prefer the smaller key pattern.
    if (index1.keyPattern.nFields() < index2.keyPattern.nFields()) {
        return kInterchangeableShorter;
    }
    return kInterchangeableLonger;
};

/*
 * Compares the proposed `newIndex` to the current set of index assignments. If we find an
 * interchangeable but worse index in the current set, we replace it with the new index. If we find
 * an interchangeable and better index, we don't add `newIndex`. If no similar indexes were found,
 * we add `newIndex` to the set.
 * Returns true if an index was pruned, otherwise false.
 */
bool pruneNewIndex(std::multiset<AssignmentAndPos, AssignmentLt>& assignmentSet,
                   OneIndexAssignment newIndex,
                   const QueryPruningInfo& queryInfo) {
    // To search the multiset we need a search key. Assignments contain a unique pointer so we have
    // to std::move newIndex into the key, do the search, then std::move back into `newIndex`.
    AssignmentAndPos searchKey = std::make_pair(std::move(newIndex), 0);
    auto [it, rangeEnd] = assignmentSet.equal_range(searchKey);
    newIndex = std::move(searchKey.first);
    const auto& newIndexEntry = (*queryInfo.indices)[newIndex.index];
    bool prunedIndex = false;
    for (; it != rangeEnd; it++) {
        const auto& existingIndex = (*it).first;
        const auto& existingIndexEntry = (*queryInfo.indices)[existingIndex.index];

        auto cmpResult = cmpIndexAssignments(newIndex, existingIndex, queryInfo);
        if (cmpResult == kInterchangeableShorter) {
            // Erase the longer index, then add the new shorter index to the set.
            LOGV2_DEBUG(8267700,
                        5,
                        "Not considering index alternative due to shorter or equal length "
                        "interchangeable index",
                        "prunedIndex"_attr = existingIndexEntry.toString(),
                        "keptIndex"_attr = newIndexEntry.toString());
            assignmentSet.erase(it);
            prunedIndex = true;
            break;
        } else if (cmpResult == kInterchangeableLonger) {
            // They are interchangeable, but the new one is longer than the existing. No need to
            // add it to the choices.
            LOGV2_DEBUG(8267701,
                        5,
                        "Not considering index alternative due to shorter or equal length "
                        "interchangeable index",
                        "prunedIndex"_attr = newIndexEntry.toString(),
                        "keptIndex"_attr = existingIndexEntry.toString());
            return true;
        }
    }

    // If we reach this point, we didn't find a better duplicate index.
    assignmentSet.insert({std::move(newIndex), assignmentSet.size()});
    return prunedIndex;
}

/*
 * Prunes the duplicate indexes from the given AndAssignment in-place. Returns true if any indexes
 * were pruned, otherwise false.
 */
bool trimAndAssignment(AndAssignment& oldAssignment, const QueryPruningInfo& queryInfo) {
    // Assignments that pass the pruning go into this set. We use a multiset because we want to only
    // compare similar indexes. std::multiset keeps data in sorted order, and our less-than function
    // compares a hash of basic index features. So we can easily iterate through the similar
    // indexes.
    // We keep the index and its original position in the AndAssignment to satisfy the potential
    // lockstep OR enumeration order.
    std::multiset<AssignmentAndPos, AssignmentLt> prunedAssignmentSet;
    // Used for assignments that aren't considered for pruning.
    std::vector<AndEnumerableState> nonPruneableChoices;
    // Keeps track of whether any indexes were pruned.
    bool prunedAnyIndexes = false;
    for (auto& choice : oldAssignment.choices) {
        if (isPruneableAndState(choice)) {
            prunedAnyIndexes |=
                pruneNewIndex(prunedAssignmentSet, std::move(choice.assignments.at(0)), queryInfo);
        } else {
            nonPruneableChoices.push_back(std::move(choice));
        }
    }

    AndAssignment replacementAssignment;
    // Add the non-pruneable choices to our new assignment.
    for (auto& choice : nonPruneableChoices) {
        replacementAssignment.choices.push_back(std::move(choice));
    }

    // The filtered assignments must be sorted in the original order we saw them in to satisfy the
    // potential lockstep OR enumeration order. This isn't expected to be expensive because we have
    // at most 64 indexes.
    std::vector<AssignmentAndPos> resultingIndexesInOrder;
    resultingIndexesInOrder.reserve(prunedAssignmentSet.size());
    extractFromSet(std::move(prunedAssignmentSet), [&](AssignmentAndPos indexAndPos) {
        resultingIndexesInOrder.push_back(std::move(indexAndPos));
    });
    std::sort(resultingIndexesInOrder.begin(),
              resultingIndexesInOrder.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });

    for (auto& indexAndPos : resultingIndexesInOrder) {
        replacementAssignment.choices.push_back(
            AndEnumerableState::makeSingleton(std::move(indexAndPos.first)));
    }

    oldAssignment = std::move(replacementAssignment);
    return prunedAnyIndexes;
}
}  // namespace

bool pruneMemoOfDupIndexes(stdx::unordered_map<MemoID, std::unique_ptr<NodeAssignment>>& memo,
                           const QueryPruningInfo& queryInfo) {
    // Iterate through each assignment in the memo and prune it. An alternative approach to this
    // might be to do a DFS, but this is simpler and yields the same result.
    bool prunedAnyIndexes = false;
    for (auto&& it : memo) {
        visit(OverloadedVisitor{
                  [&](AndAssignment& andAssignment) {
                      prunedAnyIndexes |= trimAndAssignment(andAssignment, queryInfo);
                  },
                  []<class T>(const T& otherAssignment) {},
              },
              it.second->assignment);
    }
    return prunedAnyIndexes;
}

}  // namespace plan_enumerator
}  // namespace mongo

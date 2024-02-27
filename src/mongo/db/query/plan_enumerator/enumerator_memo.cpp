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

#include "mongo/db/query/plan_enumerator/enumerator_memo.h"

namespace mongo {
namespace plan_enumerator {

bool LockstepOrAssignment::allIdentical() const {
    const auto firstCounter = subnodes[0].iterationCount;
    for (auto&& subnode : subnodes) {
        if (subnode.iterationCount != firstCounter) {
            return false;
        }
    }
    return true;
}

bool LockstepOrAssignment::shouldResetBeforeProceeding(size_t totalEnumerated,
                                                       size_t orLimit) const {
    if (totalEnumerated == 0 || !exhaustedLockstepIteration) {
        return false;
    }

    size_t totalPossibleEnumerations = 1;
    for (auto&& subnode : subnodes) {
        if (!subnode.maxIterCount) {
            return false;  // Haven't yet looped over this child entirely, not ready yet.
        }
        totalPossibleEnumerations *= subnode.maxIterCount.value();
        // If 'totalPossibleEnumerations' reaches the limit, we can just shortcut it. Otherwise,
        // 'totalPossibleEnumerations' could overflow if we have a large $or.
        if (totalPossibleEnumerations >= orLimit) {
            return false;
        }
    }

    // If we're able to compute a total number expected enumerations, we must have already cycled
    // through each of the subnodes at least once. So if we've done that and then iterated all
    // possible enumerations, we're about to repeat ourselves.
    return totalEnumerated % totalPossibleEnumerations == 0;
}

AndEnumerableState AndEnumerableState::makeSingleton(OneIndexAssignment oneIndexAssignment) {
    AndEnumerableState state;
    state.assignments.push_back(std::move(oneIndexAssignment));
    return state;
}

std::string NodeAssignment::toString() const {
    return visit(OverloadedVisitor{
                     [](const AndAssignment& andAssignment) {
                         str::stream ss;
                         ss << "AND enumstate counter " << andAssignment.counter;
                         for (size_t i = 0; i < andAssignment.choices.size(); ++i) {
                             ss << "\n\tchoice " << i << ":\n";
                             const AndEnumerableState& state = andAssignment.choices[i];
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
                     },
                     [](const ArrayAssignment& arrayAssignment) {
                         str::stream ss;
                         ss << "ARRAY SUBNODES enumstate " << arrayAssignment.counter
                            << "/ ONE OF: [ ";
                         for (size_t i = 0; i < arrayAssignment.subnodes.size(); ++i) {
                             ss << arrayAssignment.subnodes[i] << " ";
                         }
                         ss << "]";
                         return ss;
                     },
                     [](const OrAssignment& orAssignment) {
                         str::stream ss;
                         ss << "ALL OF: [ ";
                         for (size_t i = 0; i < orAssignment.subnodes.size(); ++i) {
                             ss << orAssignment.subnodes[i] << " ";
                         }
                         ss << "]";
                         return ss;
                     },
                     [](const LockstepOrAssignment& lockstepOrAssignment) {
                         str::stream ss;
                         ss << "ALL OF (lockstep): {";
                         ss << "\n\ttotalEnumerated: " << lockstepOrAssignment.totalEnumerated;
                         ss << "\n\texhaustedLockstepIteration: "
                            << lockstepOrAssignment.exhaustedLockstepIteration;
                         ss << "\n\tsubnodes: [ ";
                         for (auto&& node : lockstepOrAssignment.subnodes) {
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
                     },
                 },
                 assignment);
}

}  // namespace plan_enumerator
}  // namespace mongo

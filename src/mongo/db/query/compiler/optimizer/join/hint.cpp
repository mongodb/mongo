/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/hint.h"

namespace mongo::join_ordering {
namespace {
bool isModeValid(const SubsetLevelMode& slm) {
    return slm.mode != PlanEnumerationMode::HINTED || slm.hint;
}

/**
 * Validates that the enumeration strategy 'mode' has two properties- strictly ascending, and no two
 * consecutive modes are the same, unless the mode is HINTED.
 */
bool isEnumerationModeValid(const std::vector<SubsetLevelMode>& modes) {
    if (modes.size() < 1) {
        // Must have at least one entry.
        return false;
    }

    if (modes[0].level != 0 || !isModeValid(modes[0])) {
        // First entry must specify how we should start enumeration from the 1st subset.
        return false;
    }

    NodeSet seenNodes;
    if (modes[0].hint) {
        seenNodes.set(modes[0].hint->node);
    }

    for (size_t i = 1; i < modes.size(); i++) {
        if (!isModeValid(modes[i])) {
            return false;
        }

        if (modes[i - 1].level >= modes[i].level) {
            // Not strictly ascending.
            return false;
        }

        if (modes[i - 1].mode == PlanEnumerationMode::HINTED &&
            modes[i].level - modes[i - 1].level != 1) {
            // If previous mode is HINTED, the current level must be the previous level + 1.
            return false;
        }

        if (modes[i].mode == PlanEnumerationMode::HINTED) {
            if (seenNodes.test(modes[i].hint->node)) {
                // We can't hint on joining with the same node twice.
                return false;
            }

            seenNodes.set(modes[i].hint->node);
            continue;
        }

        if (modes[i - 1].mode == modes[i].mode) {
            // Two consecutive levels specify the same enumeration mode, and that mode isn't HINTED.
            return false;
        }
    }
    return true;
}

std::string planEnumModeToString(PlanEnumerationMode mode) {
    switch (mode) {
        case PlanEnumerationMode::CHEAPEST:
            return "CHEAPEST";
        case PlanEnumerationMode::ALL:
            return "ALL";
        case PlanEnumerationMode::HINTED:
            return "HINTED";
    }
    MONGO_UNREACHABLE_TASSERT(11458204);
}
}  // namespace

BSONObj JoinHint::toBSON() const {
    return BSON("node" << node << "method" << joinMethodToString(method) << "isLeftChild"
                       << isLeftChild);
}

BSONObj SubsetLevelMode::toBSON() const {
    BSONObjBuilder bob;
    bob << "level" << (int)level << "mode" << planEnumModeToString(mode);
    if (hint) {
        bob << "hint" << hint->toBSON();
    }
    return bob.obj();
}

PerSubsetLevelEnumerationMode::PerSubsetLevelEnumerationMode(PlanEnumerationMode mode)
    : _modes{{0, mode}} {
    tassert(11458200,
            "Only accept hinted enumeration when at least one hint is provided",
            mode != PlanEnumerationMode::HINTED);
}

PerSubsetLevelEnumerationMode::PerSubsetLevelEnumerationMode(std::vector<SubsetLevelMode> modes)
    : _modes{std::move(modes)} {
    tassert(11391600, "Expected valid enumeration mode", isEnumerationModeValid(_modes));
}

BSONObj PerSubsetLevelEnumerationMode::toBSON() const {
    BSONArrayBuilder bab;
    for (auto&& m : _modes) {
        bab << m.toBSON();
    }
    return bab.arr();
}
}  // namespace mongo::join_ordering

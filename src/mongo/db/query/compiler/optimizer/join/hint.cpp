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
bool isHintValid(const JoinHint& hint) {
    // Must specify at least one field in a hint.
    return hint.isLeftChild || hint.method || hint.node;
}

/**
 * Validates that the enumeration strategy 'mode' has two properties- strictly ascending, and no two
 * consecutive modes are the same.
 */
bool isEnumerationModeValid(const std::vector<SubsetLevelMode>& modes) {
    if (modes.size() < 1) {
        // Must have at least one entry.
        return false;
    }

    if (modes[0].level() != 0) {
        // First entry must specify how we should start enumeration from the 1st subset.
        return false;
    }

    NodeSet seenNodes;
    if (modes[0].specifiesNode()) {
        seenNodes.set(modes[0].baseNode());
    }

    for (size_t i = 1; i < modes.size(); i++) {
        if (modes[i - 1].level() >= modes[i].level()) {
            // Not strictly ascending.
            return false;
        }

        if (modes[i].specifiesHint() && modes[i].specifiesNode()) {
            if (seenNodes.test(modes[i].baseNode())) {
                // We can't hint on joining with the same node twice.
                return false;
            }

            if (modes[i].level() - modes[i - 1].level() != 1) {
                // If previous mode specifies a node, the current level must be the previous
                // level + 1.
                return false;
            }

            seenNodes.set(modes[i].baseNode());
            continue;
        }

        // Check if two consecutive levels are duplicates.
        if (modes[i - 1].mode() == modes[i].mode() &&
            modes[i - 1].specifiesHint() == modes[i].specifiesHint()) {
            if (!modes[i - 1].specifiesHint()) {
                // If neither has a hint, they're the same.
                return false;
            }
            if (modes[i].hint() == modes[i - 1].hint()) {
                // If both have the same hint, they're duplicates.
                return false;
            }
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
    }
    MONGO_UNREACHABLE_TASSERT(11458204);
}
}  // namespace

SubsetLevelMode::SubsetLevelMode(size_t level,
                                 PlanEnumerationMode mode,
                                 boost::optional<JoinHint> hint)
    : _level(level), _mode(mode), _hint(std::move(hint)) {
    if (_hint) {
        tassert(11987001, "Expected a valid hint", isHintValid(*_hint));
    }
}

BSONObj JoinHint::toBSON() const {
    BSONObjBuilder bob;
    if (node) {
        bob << "node" << *node;
    }
    if (method) {
        bob << "method" << joinMethodToString(*method);
    }
    if (isLeftChild.has_value()) {
        bob << "isLeftChild" << *isLeftChild;
    }
    return bob.obj();
}

BSONObj SubsetLevelMode::toBSON() const {
    BSONObjBuilder bob;
    bob << "level" << (int)_level << "mode" << planEnumModeToString(_mode);
    if (_hint) {
        bob << "hint" << _hint->toBSON();
    }
    return bob.obj();
}

PerSubsetLevelEnumerationMode::PerSubsetLevelEnumerationMode(PlanEnumerationMode mode)
    : _modes{{0, mode}} {}

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

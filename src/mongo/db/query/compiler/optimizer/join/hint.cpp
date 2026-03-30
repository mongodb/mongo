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

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"

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

void uassertExpectedType(const BSONElement& elem, BSONType expected) {
    uassert(12016301,
            str::stream() << "Expected " << elem.fieldName() << " to be of type "
                          << typeName(expected) << ", found " << typeName(elem.type()),
            elem.type() == expected);
}

}  // namespace

std::string planEnumModeToString(PlanEnumerationMode mode) {
    switch (mode) {
        case PlanEnumerationMode::CHEAPEST:
            return "CHEAPEST";
        case PlanEnumerationMode::ALL:
            return "ALL";
    }
    MONGO_UNREACHABLE_TASSERT(11458204);
}

PlanEnumerationMode planEnumModeFromString(const std::string& mode) {
    if (mode == "CHEAPEST") {
        return PlanEnumerationMode::CHEAPEST;
    }
    uassert(12016302, str::stream() << "Unexpected join enumeration mode " << mode, mode == "ALL");
    return PlanEnumerationMode::ALL;
}

PlanTreeShape planShapeFromString(const std::string& shape) {
    if (shape == "leftDeep") {
        return PlanTreeShape::LEFT_DEEP;
    } else if (shape == "rightDeep") {
        return PlanTreeShape::RIGHT_DEEP;
    }
    uassert(12016303, str::stream() << "Unexpected join plan shape " << shape, shape == "zigZag");
    return PlanTreeShape::ZIG_ZAG;
}

std::string planShapeToString(PlanTreeShape shape) {
    switch (shape) {
        case PlanTreeShape::LEFT_DEEP:
            return "leftDeep";
        case PlanTreeShape::RIGHT_DEEP:
            return "rightDeep";
        case PlanTreeShape::ZIG_ZAG:
            return "zigZag";
    }

    MONGO_UNREACHABLE_TASSERT(12016304);
}

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

JoinHint JoinHint::fromBSON(const BSONObj& obj) {
    JoinHint hint;
    for (auto elem : obj) {
        if (elem.fieldNameStringData() == "node") {
            uassertExpectedType(elem, BSONType::numberInt);
            auto i = elem.numberInt();
            uassert(12016305, "Expectected 'node' to be non-negative", i >= 0);
            hint.node = i;
        } else if (elem.fieldNameStringData() == "method") {
            uassertExpectedType(elem, BSONType::string);
            hint.method = joinMethodFromString(elem.String());
        } else if (elem.fieldNameStringData() == "isLeftChild") {
            uassertExpectedType(elem, BSONType::boolean);
            hint.isLeftChild = elem.boolean();
        } else {
            uasserted(12016306,
                      str::stream()
                          << "Unexpected field '" << elem.fieldName() << "' for join hint.");
        }
    }
    uassert(12016307, "Provided hint was not valid", isHintValid(hint));
    return hint;
}

BSONObj SubsetLevelMode::toBSON() const {
    BSONObjBuilder bob;
    bob << "level" << (int)_level << "mode" << planEnumModeToString(_mode);
    if (_hint) {
        bob << "hint" << _hint->toBSON();
    }
    return bob.obj();
}

SubsetLevelMode SubsetLevelMode::fromBSON(const BSONObj& obj) {
    boost::optional<size_t> level;
    boost::optional<PlanEnumerationMode> mode;
    boost::optional<JoinHint> hint;
    for (auto elem : obj) {
        if (elem.fieldNameStringData() == "level") {
            uassertExpectedType(elem, BSONType::numberInt);
            auto i = elem.numberInt();
            uassert(12016308, "Expectected 'level' to be non-negative", i >= 0);
            level = i;
        } else if (elem.fieldNameStringData() == "mode") {
            uassertExpectedType(elem, BSONType::string);
            mode = planEnumModeFromString(elem.String());
        } else if (elem.fieldNameStringData() == "hint") {
            uassertExpectedType(elem, BSONType::object);
            hint = JoinHint::fromBSON(elem.Obj());
        } else {
            uasserted(12016309,
                      str::stream() << "Unexpected field '" << elem.fieldNameStringData()
                                    << "' for subset level mode.");
        }
    }

    uassert(12016310, "", mode && level);
    return SubsetLevelMode(*level, *mode, std::move(hint));
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

PerSubsetLevelEnumerationMode PerSubsetLevelEnumerationMode::fromBSON(const BSONObj& obj) {
    std::vector<SubsetLevelMode> modes;
    for (auto elem : obj) {
        uassertExpectedType(elem, BSONType::object);
        modes.push_back(SubsetLevelMode::fromBSON(elem.Obj()));
    }
    uassert(12016311, "Expected valid enumeration mode", isEnumerationModeValid(modes));
    return {modes};
}

BSONObj EnumerationStrategy::toBSON() const {
    return BSON("planShape" << planShapeToString(planShape) << "perSubsetLevelMode" << mode.toBSON()
                            << "enableHJOrderPruning" << enableHJOrderPruning);
}

EnumerationStrategy EnumerationStrategy::fromBSON(const BSONObj& obj) {
    PlanTreeShape planShape = PlanTreeShape::ZIG_ZAG;
    bool enableHJOrderPruning = false;
    boost::optional<PerSubsetLevelEnumerationMode> mode;

    uassert(12016312, "Expected field to be set in enumeration strategy", !obj.isEmpty());
    for (auto elem : obj) {
        if (elem.fieldNameStringData() == "planShape") {
            uassertExpectedType(elem, BSONType::string);
            planShape = planShapeFromString(elem.String());
        } else if (elem.fieldNameStringData() == "perSubsetLevelMode") {
            uassert(12016317,
                    str::stream() << "Expected 'perSubsetLevelMode' to be an array",
                    elem.isABSONObj());
            auto obj = elem.Obj();
            mode = PerSubsetLevelEnumerationMode::fromBSON(obj);
        } else if (elem.fieldNameStringData() == "enableHJOrderPruning") {
            uassert(12016313,
                    "Expected 'enableHJOrderPruning' value to be a boolean",
                    elem.type() == BSONType::boolean);
            enableHJOrderPruning = elem.boolean();
        } else {
            uasserted(12016314,
                      str::stream() << "Unexpected field '" << elem.fieldNameStringData()
                                    << "' for enumeration strategy");
        }
    }

    if (!mode) {
        mode = PerSubsetLevelEnumerationMode(PlanEnumerationMode::CHEAPEST);
    }

    return EnumerationStrategy{planShape, std::move(*mode), enableHJOrderPruning};
}

}  // namespace mongo::join_ordering

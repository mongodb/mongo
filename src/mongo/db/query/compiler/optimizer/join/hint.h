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

#pragma once

#include "mongo/db/query/compiler/optimizer/join/join_method.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/util/modules.h"
namespace mongo::join_ordering {

/**
 * Describes shape of plan tree.
 */
enum class PlanTreeShape { LEFT_DEEP, RIGHT_DEEP, ZIG_ZAG };

/**
 * Helpers to serialize/deserialize a plan tree shape. Deserialization uasserts if string is not a
 * valid plan tree shape.
 */
std::string planShapeToString(PlanTreeShape mode);
PlanTreeShape planShapeFromString(const std::string& mode);

/**
 * Determines what plans we enumerate.
 */
enum class PlanEnumerationMode {
    // Only enumerate plans if they are cheaper than the lowest-cost plan for each subset.
    CHEAPEST,
    // Enumerates all plans, regardless of cost.
    ALL
};

/**
 * Helpers to serialize/deserialize a plan enumeration mode. Deserialization uasserts if string is
 * not a valid mode.
 */
std::string planEnumModeToString(PlanEnumerationMode mode);
PlanEnumerationMode planEnumModeFromString(const std::string& mode);

/**
 * Hints how a join should be done at the current subset level. All hints are optional- when not
 * specified, the choice is up to the plan enumerator.
 */
struct JoinHint {
    // The next node to join with.
    boost::optional<NodeId> node;
    // The next join method to use. Ignored for base subset.
    boost::optional<JoinMethod> method;
    /**
     * Indicates if the base collection access node is the left or right child of this node.
     * Ignored for base subset. For example, this hint vector (let every entry indicate one subset
     * level):
     *   {{INLJ, 1, true}, {HJ, 2, true}, {NLJ, 3, false}}
     * would result in the following tree:
     *
     *          NLJ
     *         /   \
     *        HJ    3
     *       /  \
     *      2    1
     *
     * We ignore the join method/isLeftChild field for the first level (node 1). We place node 2 on
     * the left side of a HJ ('isLeftChild' = true), and our current subtree on the right. Finally,
     * we place an NLJ with node 3 on the right ('isLeftChild' = false).
     */
    boost::optional<bool> isLeftChild;

    BSONObj toBSON() const;
    static JoinHint fromBSON(const BSONObj& obj);

    bool operator==(const JoinHint& other) const {
        return node == other.node && method == other.method && isLeftChild == other.isLeftChild;
    }
};

/**
 * Describes enumeration strategy for a given subset level and above.
 */
class SubsetLevelMode {
public:
    SubsetLevelMode(size_t level,
                    PlanEnumerationMode mode,
                    boost::optional<JoinHint> hint = boost::none);

    inline bool specifiesNode() const {
        return _hint && _hint->node.has_value();
    }

    inline bool canBaseNodeBe(NodeId node) const {
        return !_hint || !_hint->node || (*_hint->node == node);
    }

    inline bool baseNodeCanBeOnLeft() const {
        if (!_hint || !_hint->isLeftChild.has_value()) {
            return true;
        }
        return _hint->isLeftChild.get();
    }

    inline bool baseNodeCanBeOnRight() const {
        if (!_hint || !_hint->isLeftChild.has_value()) {
            return true;
        }
        return !_hint->isLeftChild.get();
    }

    inline bool canMethodBe(JoinMethod method) const {
        return !_hint || !_hint->method.has_value() || (*_hint->method == method);
    }

    inline size_t level() const {
        return _level;
    }

    inline PlanEnumerationMode mode() const {
        return _mode;
    }

    inline bool specifiesHint() const {
        return _hint.has_value();
    }

    inline NodeId baseNode() const {
        tassert(11987002, "Expected a node to be specified", specifiesNode());
        return *_hint->node;
    }

    inline JoinMethod method() const {
        tassert(11987003,
                "Expected a join method to be specified",
                specifiesHint() && _hint->method.has_value());
        return *_hint->method;
    }

    inline bool baseNodeOnLeft() const {
        tassert(11987004,
                "Expected a node side to be specified",
                specifiesHint() && _hint->isLeftChild.has_value());
        return *_hint->isLeftChild;
    }

    inline const JoinHint& hint() const {
        tassert(11987005, "Expected a hint to be specified", specifiesHint());
        return *_hint;
    }

    BSONObj toBSON() const;
    static SubsetLevelMode fromBSON(const BSONObj& obj);

private:
    // First level at which to apply this mode.
    size_t _level;
    PlanEnumerationMode _mode;
    // Used to specify what we should enumerate for this subset level.
    boost::optional<JoinHint> _hint = boost::none;
};

/**
 * This structure allows us to specify a particular enumeration mode per subset level. Note that:
 *  - A mode must always be specified for level 0.
 *  - It is not permitted to specify the same exact mode for two consecutive entries.
 *
 * The default mode is:
 *  {{0, CHEAPEST}}
 *
 * This means that for all subset levels (including 0), we will use the "CHEAPEST" enumeration mode.
 *
 * Modes are "sticky" until a the next entry specifying a new mode for a level is found, i.e. levels
 * keep using the mode last specified for the previous level unless there is an entry specifically
 * for that level. For example:
 *  {{0, CHEAPEST}, {2, ALL}, {4, CHEAPEST}}
 *
 * For subset levels 0 & 1, we will apply the "CHEAPEST" enumeration mode. Then, for subsets 2 & 3,
 * we will apply all plans enumeration (ALL). Finally, for any subset level 4+, we go back to
 * picking the cheapest subset.
 */
class PerSubsetLevelEnumerationMode {
public:
    PerSubsetLevelEnumerationMode(PlanEnumerationMode mode);
    PerSubsetLevelEnumerationMode(std::vector<SubsetLevelMode> modes);

    struct Iterator {
        Iterator& next() {
            if (_index < _mode._modes.size()) {
                _index++;
            }
            return *this;
        }

        bool operator==(const Iterator& other) const {
            tassert(
                11391603, "Must be comparing iterators on same instance", &_mode == &other._mode);
            return _index == other._index;
        }

        auto get() const {
            tassert(11391604, "Must not be end iterator", _index < _mode._modes.size());
            return _mode._modes[_index];
        }

    private:
        Iterator(const PerSubsetLevelEnumerationMode& mode, size_t index)
            : _mode{mode}, _index{index} {}

        const PerSubsetLevelEnumerationMode& _mode;
        size_t _index;
        friend PerSubsetLevelEnumerationMode;
    };

    Iterator begin() const {
        return Iterator(*this, 0);
    };

    Iterator end() const {
        return Iterator(*this, _modes.size());
    };

    BSONObj toBSON() const;
    static PerSubsetLevelEnumerationMode fromBSON(const BSONObj& obj);

private:
    std::vector<SubsetLevelMode> _modes;
    friend PerSubsetLevelEnumerationMode::Iterator;
};

/**
 * This configures the kinds of plans we're generating and how we're choosing between them during
 * enumeration.
 */
struct EnumerationStrategy {
    PlanTreeShape planShape;
    PerSubsetLevelEnumerationMode mode;
    bool enableHJOrderPruning;

    static EnumerationStrategy fromBSON(const BSONObj& obj);
    BSONObj toBSON() const;
};

}  // namespace mongo::join_ordering

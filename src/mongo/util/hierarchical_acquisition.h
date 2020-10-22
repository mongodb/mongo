/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <climits>
#include <cstdint>

#include "mongo/util/assert_util.h"

namespace mongo {

namespace hierarchical_acquisition_detail {

/**
 * Hierarchical acquisition types are light-weight wrappers around bitwise math
 *
 * Each Level is a bitmap with a single bit set. The corresponding IndexType index for each Level is
 * how many bits that level's bit is right shifted from the greatest bit in the internal ValueType
 * value. So a Level 0 (L0) will be the bitmap 0X8000000000000000000000000. Likewise, a Level 63
 * (L63) bitmap will be 0X0000000000000000000000001. This provides a fairly obvious mathematical
 * property:
 *   La < Lb <=> value(La) > (Lb)
 *
 * Each Set is a more classical bitmap. When a Level is added to a Set, the Level's bit is set in
 * the Set's underlying value. When a Level is removed from a Set, the Level's bit is removed from
 * the Set's underlying value. The rule is that each Level added to a Set must be less than any
 * previously added Level. Likewise, each Level removed from a Set must be less than any
 * remaining Level in the Set. Translated to math, the following is a pre-condition for adding a
 * level L to a set S and a post-condition for removing a level L to a set S:
 *   value(L) > value(S)
 *
 * In the interest of allowing the developer more flexibility and keeping these types simple, Set
 * functions do not throw or return ErrorCodes, instead they return small enums. Note that
 * operations always go through, even if the result is kInvalid.
 */

using ValueType = uint64_t;

class Set;

/**
 * Level is a constexpr type that encodes an integer value into a bitmap
 *
 * This class is designed for use in non-type template parameters like so:
 * template<Level kLevel>
 * struct Foo {
 *   void bar() {
 *     gSet.add(kLevel);
 *   }
 * };
 *
 * Note that LT/LTE/GT/GTE operators are internally reversed because a low Level means a high
 * ValueType.
 */
class Level {
    friend class Set;

public:
    using IndexType = int32_t;

    static constexpr IndexType kMinIndex = 0;
    static constexpr IndexType kMaxIndex = sizeof(ValueType) * CHAR_BIT - 1;

    explicit constexpr Level(IndexType index) : Level(maxValue() >> index, index) {}

    constexpr Level prevLevel() const {
        return Level(_value << 1);
    }
    constexpr Level nextLevel() const {
        return Level(_value >> 1);
    }

    constexpr IndexType index() const {
        return _index;
    }

    constexpr friend bool operator<(const Level& lhs, const Level& rhs) {
        return lhs._value > rhs._value;
    }

    constexpr friend bool operator<=(const Level& lhs, const Level& rhs) {
        return lhs._value >= rhs._value;
    }

    constexpr friend bool operator>(const Level& lhs, const Level& rhs) {
        return lhs._value < rhs._value;
    }

    constexpr friend bool operator>=(const Level& lhs, const Level& rhs) {
        return lhs._value <= rhs._value;
    }

    constexpr friend bool operator==(const Level& lhs, const Level& rhs) {
        return lhs._value == rhs._value;
    }

    constexpr friend bool operator!=(const Level& lhs, const Level& rhs) {
        return lhs._value != rhs._value;
    }

private:
    static constexpr ValueType minValue() {
        return ValueType{0x1} << kMinIndex;
    }
    static constexpr ValueType maxValue() {
        return ValueType{0x1} << kMaxIndex;
    }

    explicit constexpr Level(ValueType value, IndexType index) : _value(value), _index(index) {
        invariantForConstexpr(_value >= minValue());
        invariantForConstexpr(_value <= maxValue());
    }

    ValueType _value;
    IndexType _index;
};

/**
 * Set is a container type that adds or removes Levels
 */
class Set {
public:
    enum class AddResult {
        kValidWasAbsent,
        kInvalidWasAbsent,
        kInvalidWasPresent,
    };

    friend StringData toString(AddResult result) {
        switch (result) {
            case AddResult::kValidWasAbsent: {
                return "ValidWasAbsent"_sd;
            } break;
            case AddResult::kInvalidWasAbsent: {
                return "InvalidWasAbsent"_sd;
            } break;
            case AddResult::kInvalidWasPresent: {
                return "InvalidWasPresent"_sd;
            } break;
        }

        MONGO_UNREACHABLE;
    }

    friend std::ostream& operator<<(std::ostream& os, const AddResult& result) {
        return os << toString(result);
    }

    enum class RemoveResult {
        kValidWasPresent,
        kInvalidWasPresent,
        kInvalidWasAbsent,
    };

    friend StringData toString(RemoveResult result) {
        switch (result) {
            case RemoveResult::kValidWasPresent: {
                return "ValidWasPresent"_sd;
            } break;
            case RemoveResult::kInvalidWasPresent: {
                return "InvalidWasPresent"_sd;
            } break;
            case RemoveResult::kInvalidWasAbsent: {
                return "InvalidWasAbsent"_sd;
            } break;
        };

        MONGO_UNREACHABLE;
    }

    friend std::ostream& operator<<(std::ostream& os, const RemoveResult& result) {
        return os << toString(result);
    }

    /**
     * Check if this set has a Level
     */
    bool has(const Level& level) const {
        return _value & level._value;
    }

    /**
     * Add a Level to this set
     *
     * There are three possible outcomes:
     * - If level is already in the set, then return kInvalidWasPresent.
     * - If level is not in the set and less than any level in the set, then add the level to the
     *   set and return kInvalidWasAbsent.
     * - If level is not in the set and greater than any level in the set, then add the level to the
     *   set and return kValidWasAbsent.
     */
    auto add(const Level& level) {
        if (_value & level._value) {
            return AddResult::kInvalidWasPresent;
        }

        auto oldValue = _value;
        _value |= level._value;

        if (level._value < oldValue) {
            return AddResult::kInvalidWasAbsent;
        }

        return AddResult::kValidWasAbsent;
    }

    /**
     * Remove a Level from this set
     *
     * There are three possible outcomes:
     * - If level is not in the set, then return kInvalidWasAbsent
     * - If level is in the set and less than any other level in the set, then remove the level from
     *   the set and return kInvalidWasPresent
     * - If level is in the set and greater than any level in the set, then remove the level from
     *   the set and return kValidWasPresent
     */
    auto remove(const Level& level) {
        if (~_value & level._value) {
            return RemoveResult::kInvalidWasAbsent;
        }

        _value &= ~level._value;

        if (level._value < _value) {
            return RemoveResult::kInvalidWasPresent;
        }

        return RemoveResult::kValidWasPresent;
    }

private:
    ValueType _value = 0;
};

}  // namespace hierarchical_acquisition_detail

using HierarchicalAcquisitionSet = hierarchical_acquisition_detail::Set;
using HierarchicalAcquisitionLevel = hierarchical_acquisition_detail::Level;

}  // namespace mongo

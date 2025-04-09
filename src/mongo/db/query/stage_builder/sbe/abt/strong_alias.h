/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::abt {

/**
 * Strong string alias. It is used to provide strong type safety between various string types in the
 * optimizer code. Instances with different tags are not constructable from, assignable to and
 * comparable to each other. TagType needs to define a constexpr boolean "kAllowEmpty" which
 * determines if empty strings are allowed.
 */
template <class TagType>
class StrongStringAlias {
public:
    struct Hasher {
        size_t operator()(const StrongStringAlias& v) const {
            return std::hash<std::string>()(v._value);
        }
    };

    // Allow implicit construction from a string literal, but not from a const char*.
    template <size_t N>
    StrongStringAlias(const char (&value)[N]) : _value(value){};

    // We disallow empty strings based on the tag's kAllowEmpty field.
    template <class TagType1 = TagType, class = typename std::enable_if_t<!TagType1::kAllowEmpty>>
    StrongStringAlias(const char (&value)[1]) = delete;

    // Need to explicitly construct from StringData, const char*, or std::string.
    explicit StrongStringAlias(std::string value) : _value(std::move(value)) {
        if constexpr (!TagType::kAllowEmpty) {
            invariant(!_value.empty());
        }
    }

    explicit StrongStringAlias(StringData value) : _value(value) {
        if constexpr (!TagType::kAllowEmpty) {
            invariant(!_value.empty());
        }
    }

    StrongStringAlias(const StrongStringAlias<TagType>& other) = default;
    StrongStringAlias(StrongStringAlias<TagType>&& other) = default;
    StrongStringAlias& operator=(const StrongStringAlias& other) = default;
    StrongStringAlias& operator=(StrongStringAlias&& other) = default;

    bool operator==(const StrongStringAlias& other) const {
        return _value == other._value;
    }
    bool operator!=(const StrongStringAlias& other) const {
        return _value != other._value;
    }

    bool operator<(const StrongStringAlias& other) const {
        return _value < other._value;
    }

    int compare(const StrongStringAlias& other) const {
        return _value.compare(other._value);
    }

    template <size_t N>
    bool operator==(const char (&value)[N]) const {
        return _value == value;
    }
    template <size_t N>
    bool operator!=(const char (&value)[N]) const {
        return _value != value;
    }

    StringData value() const {
        return _value;
    }

private:
    std::string _value;
};

/**
 * Exclude str::stream since it currently leads to ambiguous calls.
 */
template <class StreamType,
          class TagType,
          class = typename std::enable_if_t<!std::is_same<StreamType, str::stream>::value>>
StreamType& operator<<(StreamType& stream, const StrongStringAlias<TagType>& t) {
    return stream << t.value();
}

/**
 * Strong double alias. Used for cardinality estimation and selectivity. The tag type is expected to
 * have a boolean field "kUnitless". It specifies if this entity is unitless (e.g. a simple ratio, a
 * percent) vs having units (e.g. documents). This effectively enables or disables multiplication
 * and division by the same alias type.
 */
template <class TagType>
struct StrongDoubleAlias {
    struct Hasher {
        size_t operator()(const StrongDoubleAlias& v) const {
            return std::hash<double>()(v._value);
        }
    };

    explicit operator double() const {
        return _value;
    }

    constexpr void assertValid() const {
        uassert(7180104, "Invalid value", _value >= TagType::kMinValue);
        uassert(7180105, "Invalid value", _value <= TagType::kMaxValue);
    }

    // Prevent implicit conversion from bool to double.
    template <typename T>
    StrongDoubleAlias(T)
    requires std::is_same_v<T, bool>
    = delete;

    constexpr StrongDoubleAlias(const double value) : _value(value) {
        assertValid();
    }

    constexpr StrongDoubleAlias() = default;

    constexpr bool operator==(const StrongDoubleAlias other) const {
        return _value == other._value;
    }
    constexpr bool operator!=(const StrongDoubleAlias other) const {
        return _value != other._value;
    }
    constexpr bool operator>(const StrongDoubleAlias other) const {
        return _value > other._value;
    }
    constexpr bool operator>=(const StrongDoubleAlias other) const {
        return _value >= other._value;
    }
    constexpr bool operator<(const StrongDoubleAlias other) const {
        return _value < other._value;
    }
    constexpr bool operator<=(const StrongDoubleAlias other) const {
        return _value <= other._value;
    }

    constexpr bool operator==(const double other) const {
        return _value == other;
    }
    constexpr bool operator!=(const double other) const {
        return _value != other;
    }
    constexpr bool operator>(const double other) const {
        return _value > other;
    }
    constexpr bool operator>=(const double other) const {
        return _value >= other;
    }
    constexpr bool operator<(const double other) const {
        return _value < other;
    }
    constexpr bool operator<=(const double other) const {
        return _value <= other;
    }

    constexpr StrongDoubleAlias operator+(const StrongDoubleAlias other) const {
        return {_value + other._value};
    }
    constexpr StrongDoubleAlias operator-(const StrongDoubleAlias other) const {
        return {_value - other._value};
    }

    constexpr StrongDoubleAlias& operator+=(const StrongDoubleAlias other) {
        _value += other._value;
        return *this;
    }
    constexpr StrongDoubleAlias& operator-=(const StrongDoubleAlias other) {
        _value -= other._value;
        return *this;
    }

    // We specifically disallow adding and subtracting a double.

    constexpr StrongDoubleAlias operator*(const double other) const {
        return {_value * other};
    }
    constexpr StrongDoubleAlias operator/(const double other) const {
        return {_value / other};
    }

    constexpr StrongDoubleAlias& operator*=(const double other) {
        _value *= other;
        return *this;
    }
    constexpr StrongDoubleAlias& operator/=(const double other) {
        _value /= other;
        return *this;
    }

    constexpr StrongDoubleAlias pow(const double exponent) const {
        return {std::pow(_value, exponent)};
    }


    /**
     * We specifically do not add allow multiplication and addition with the same type. If we have
     * units (such as in CE: documents), this violates the unit preservation invariant: e.g. when we
     * expect a number of apples we do not want to get an apple^2 entity. Multiplication and
     * division need to be specifically allowed via overloads.
     */
    template <class TagType1, class = typename std::enable_if_t<TagType1::kUnitless>>
    constexpr StrongDoubleAlias<TagType1> operator*(const StrongDoubleAlias<TagType1> other) const {
        return {_value * other._value};
    }
    template <class TagType1, class = typename std::enable_if_t<TagType1::kUnitless>>
    constexpr StrongDoubleAlias<TagType1>& operator*=(const StrongDoubleAlias<TagType1> other) {
        _value *= other._value;
        return *this;
    }

    template <class TagType1, class = typename std::enable_if_t<TagType1::kUnitless>>
    constexpr StrongDoubleAlias<TagType1> operator/(const StrongDoubleAlias<TagType1> other) const {
        return {_value / other._value};
    }
    template <class TagType1, class = typename std::enable_if_t<TagType1::kUnitless>>
    constexpr StrongDoubleAlias<TagType1>& operator/=(const StrongDoubleAlias<TagType1> other) {
        _value /= other._value;
        return *this;
    }

    double _value;
};

/**
 * Utility functions.
 */
template <class StreamType, class TagType>
StreamType& operator<<(StreamType& stream, StrongDoubleAlias<TagType> t) {
    return stream << t._value;
}

template <class T>
constexpr StrongDoubleAlias<T> operator*(const double v1, StrongDoubleAlias<T> v2) {
    return v2 * v1;
}

}  // namespace mongo::abt

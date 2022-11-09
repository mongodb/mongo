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


namespace mongo::optimizer {

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
    template <class TagType1 = TagType,
              class = typename std::enable_if<!TagType1::kAllowEmpty>::type>
    StrongStringAlias(const char (&value)[1]) = delete;

    // Need to explicitly construct from StringData and const char*.
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
          class = typename std::enable_if<!std::is_same<StreamType, str::stream>::value>::type>
StreamType& operator<<(StreamType& stream, const StrongStringAlias<TagType>& t) {
    return stream << t.value();
}

}  // namespace mongo::optimizer

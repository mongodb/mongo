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

#include "mongo/util/fixed_string.h"

#include <fmt/format.h>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(FixedString, ConversionToStringData) {
    static constexpr FixedString hi{"hi"};
    static_assert(std::is_same_v<decltype(hi), const FixedString<2>>);
    ASSERT_EQ(StringData{hi}, "hi");
}

TEST(FixedString, CharListCtor) {
    static constexpr FixedString hi{'h', 'i'};
    static_assert(std::is_same_v<decltype(hi), const FixedString<2>>);
    ASSERT_EQ(StringData{hi}, "hi");
}

template <FixedString fs>
struct HasStringParam {
    static constexpr const decltype(fs)& string() {
        return fs;
    }
};

TEST(FixedString, StringTemplateParameter) {
    ASSERT_EQ(HasStringParam<"hi">::string(), "hi");
}

TEST(FixedString, Plus) {
    ASSERT_EQ(FixedString{"abc"} + FixedString{"xyz"}, "abcxyz");

    ASSERT_EQ(FixedString{"abc"} + "xyz", "abcxyz");
    ASSERT_EQ("xyz" + FixedString{"abc"}, "xyzabc");

    ASSERT_EQ(FixedString{"abc"} + 'x', "abcx");
    ASSERT_EQ('x' + FixedString{"abc"}, "xabc");
}

TEST(FixedString, EmbeddedNul) {
    ASSERT_EQ(FixedString{"abc\0d"}, "abc\0d"_sd);
    ASSERT_EQ((FixedString{'a', 'b', 'c', '\0', 'd'}), "abc\0d"_sd);
}

/**
 * Given tuples aTup and bTup, computes a tuple of 2-tuples:
 * {a0...aN} X {b0...bN} => {
 *   {a0,b0}, {a0,b1}, ... {a0,bN},
 *   {a1,b0}, {a1,b1}, ... {a1,bN},
 *   ...
 *   {aN,b0}, {aN,b1}, ... {aN,bN},
 * }
 */
constexpr auto tupleCartesianProduct(auto aTup, auto bTup) {
    return std::apply(
        [&](auto... as) {
            return std::tuple_cat([&](auto a) {
                return std::apply(
                    [&](auto... bs) {
                        return std::tuple{std::tuple{a, bs}...};
                    },
                    bTup);
            }(as)...);
        },
        aTup);
}

TEST(FixedString, Comparison) {
    // Test every element of `strs` with every other element of `strs`.
    // Each such test performs all 6 comparisons, and we make sure that we get
    // the same answer as their StringData representation would.
    // This has to be tuple-based because the FixedStrings are all
    // different types.
    static constexpr std::tuple strs{
        FixedString{"a"},
        FixedString{"b"},
        FixedString{"b\0"},
        FixedString{"ba"},
        FixedString{"bb"},
        FixedString{"c"},
    };
    auto doCompare = [](const auto& a, const auto& b) {
        std::cout << "(" << str::escape(a) << ", " << str::escape(b) << ")\n";
        ASSERT_EQ(a == b, StringData{a} == StringData{b});
        ASSERT_EQ(a != b, StringData{a} != StringData{b});
        ASSERT_EQ(a < b, StringData{a} < StringData{b});
        ASSERT_EQ(a > b, StringData{a} > StringData{b});
        ASSERT_EQ(a <= b, StringData{a} <= StringData{b});
        ASSERT_EQ(a >= b, StringData{a} >= StringData{b});
    };
    std::apply([&](auto... pairs) { (std::apply(doCompare, pairs), ...); },
               tupleCartesianProduct(strs, strs));
}

template <FixedString name, typename Rep>
struct UnitQuantity {
    static constexpr const auto& unitName() {
        return name;
    }

    std::string toString() const {
        return fmt::format("{} {}", value, StringData{unitName()});
    }

    Rep value;
};

using Degrees = UnitQuantity<"degrees", double>;
using Radians = UnitQuantity<"radians", double>;
using Bytes = UnitQuantity<"bytes", uint64_t>;

constexpr inline FixedString megaPrefix{"mega"};
using Megabytes = UnitQuantity<megaPrefix + Bytes::unitName(), uint64_t>;

TEST(FixedString, DocExample) {
    ASSERT_EQ(Degrees{180}.toString(), "180 degrees");
    ASSERT_EQ(Radians{3.14}.toString(), "3.14 radians");
    ASSERT_EQ(Megabytes{123}.toString(), "123 megabytes");
}

}  // namespace
}  // namespace mongo

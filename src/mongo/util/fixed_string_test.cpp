// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/fixed_string.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(FixedString, ConversionToStringData) {
    static constexpr FixedString hi{"hi"};
    static_assert(std::is_same_v<decltype(hi), const FixedString<2>>);
    ASSERT_EQ(std::string_view{hi}, "hi");
}

TEST(FixedString, CharListCtor) {
    static constexpr FixedString hi{'h', 'i'};
    static_assert(std::is_same_v<decltype(hi), const FixedString<2>>);
    ASSERT_EQ(std::string_view{hi}, "hi");
}

template <FixedString fs>
struct HasStringParam {
    static constexpr const decltype(fs)& string() {
        return fs;
    }
};

TEST(FixedString, StringTemplateParameter) {
    ASSERT_EQ(HasStringParam<"hi">::string(), FixedString{"hi"});
}

TEST(FixedString, Plus) {
    ASSERT_EQ(FixedString{"abc"} + FixedString{"xyz"}, FixedString{"abcxyz"});

    ASSERT_EQ(FixedString{"abc"} + "xyz", FixedString{"abcxyz"});
    ASSERT_EQ("xyz" + FixedString{"abc"}, FixedString{"xyzabc"});

    ASSERT_EQ(FixedString{"abc"} + 'x', FixedString{"abcx"});
    ASSERT_EQ('x' + FixedString{"abc"}, FixedString{"xabc"});
}

TEST(FixedString, EmbeddedNul) {
    ASSERT_EQ(FixedString{"abc\0d"}, "abc\0d"sv);
    ASSERT_EQ((FixedString{'a', 'b', 'c', '\0', 'd'}), "abc\0d"sv);
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
                return std::apply([&](auto... bs) { return std::tuple{std::tuple{a, bs}...}; },
                                  bTup);
            }(as)...);
        },
        aTup);
}

TEST(FixedString, Comparison) {
    // Test every element of `strs` with every other element of `strs`.
    // Each such test performs all 6 comparisons, and we make sure that we get
    // the same answer as their std::string_view representation would.
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
    std::apply(
        [&](auto... pairs) {
            (std::apply(
                 [](auto&& a, auto&& b) {
                     std::string_view sa{a};
                     std::string_view sb{b};
                     SCOPED_TRACE(fmt::format("{}={:?}", "a", sa));
                     SCOPED_TRACE(fmt::format("{}={:?}", "b", sb));
                     EXPECT_EQ(a == b, sa == sb);
                     EXPECT_EQ(a != b, sa != sb);
                     EXPECT_EQ(a < b, sa < sb);
                     EXPECT_EQ(a > b, sa > sb);
                     EXPECT_EQ(a <= b, sa <= sb);
                     EXPECT_EQ(a >= b, sa >= sb);
                 },
                 pairs),
             ...);
        },
        tupleCartesianProduct(strs, strs));
}

template <FixedString name, typename Rep>
struct UnitQuantity {
    static constexpr const auto& unitName() {
        return name;
    }

    std::string toString() const {
        return fmt::format("{} {}", value, std::string_view{unitName()});
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

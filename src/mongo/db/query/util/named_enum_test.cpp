// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/named_enum.h"

#include "mongo/unittest/unittest.h"

#include <iterator>
#include <string_view>
namespace mongo {
namespace {

// An entry without an explicit name reports the enumerator itself.
#define NAMED_ENUM_TEST_PLAIN_TABLE(X) \
    X(red)                             \
    X(green)                           \
    X(blue)
QUERY_UTIL_NAMED_ENUM_DEFINE(TestPlainColors, NAMED_ENUM_TEST_PLAIN_TABLE)
#undef NAMED_ENUM_TEST_PLAIN_TABLE

// An entry may supply a name that differs from the enumerator, including one that is not derivable
// from it by any mechanical transformation ("EOF", "sky blue").
#define NAMED_ENUM_TEST_NAMED_TABLE(X) \
    X(kRed, "red")                     \
    X(kBlue, "sky blue")               \
    X(kEof, "EOF")
QUERY_UTIL_NAMED_ENUM_DEFINE(TestNamedColors, NAMED_ENUM_TEST_NAMED_TABLE)
#undef NAMED_ENUM_TEST_NAMED_TABLE

// Both entry forms may be mixed within one table.
#define NAMED_ENUM_TEST_MIXED_TABLE(X) \
    X(red)                             \
    X(kGreen, "green")                 \
    X(blue)
QUERY_UTIL_NAMED_ENUM_DEFINE(TestMixedColors, NAMED_ENUM_TEST_MIXED_TABLE)
#undef NAMED_ENUM_TEST_MIXED_TABLE

TEST(NamedEnumTest, PlainEntriesReportEnumeratorNames) {
    ASSERT_EQ(toStringData(TestPlainColors::red), "red");
    ASSERT_EQ(toStringData(TestPlainColors::green), "green");
    ASSERT_EQ(toStringData(TestPlainColors::blue), "blue");
}

TEST(NamedEnumTest, NamedEntriesReportTheSuppliedName) {
    ASSERT_EQ(toStringData(TestNamedColors::kRed), "red");
    ASSERT_EQ(toStringData(TestNamedColors::kBlue), "sky blue");
    ASSERT_EQ(toStringData(TestNamedColors::kEof), "EOF");
}

TEST(NamedEnumTest, MixedTableReportsEachEntryPerItsOwnForm) {
    ASSERT_EQ(toStringData(TestMixedColors::red), "red");
    ASSERT_EQ(toStringData(TestMixedColors::kGreen), "green");
    ASSERT_EQ(toStringData(TestMixedColors::blue), "blue");
}

TEST(NamedEnumTest, EnumeratorsAreContiguousFromZeroAndNamesStayAligned) {
    // toStringData() indexes the name array by the enumerator's value, so the two must stay in
    // lockstep. A named entry must contribute exactly one enumerator, with its name in the same
    // position - not, say, leak its name into the enum.
    ASSERT_EQ(static_cast<size_t>(TestNamedColors::kRed), 0u);
    ASSERT_EQ(static_cast<size_t>(TestNamedColors::kBlue), 1u);
    ASSERT_EQ(static_cast<size_t>(TestNamedColors::kEof), 2u);
    ASSERT_EQ(std::size(TestNamedColorsEnumString::arr_), 3u);
}

TEST(NamedEnumTest, NamesAreUsableInConstantExpressions) {
    static_assert(toStringData(TestNamedColors::kEof) == std::string_view{"EOF"});
    static_assert(toStringData(TestMixedColors::red) == std::string_view{"red"});
}

}  // namespace
}  // namespace mongo

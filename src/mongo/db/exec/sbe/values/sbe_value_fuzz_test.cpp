// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/value.h"

#include <cstdint>
#include <limits>
#include <variant>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace mongo::sbe {
namespace {

struct Null {};
struct Nothing {};

// C++ analogue of the JS getScalarArb: int, bool, null, nothing.
using SBEScalarSpec = std::variant<Null,     // Null
                                   Nothing,  // Nothing (missing value)
                                   bool,     // Boolean
                                   int32_t,  // NumberInt32
                                   int64_t,  // NumberInt64
                                   double>;  // NumberDouble

auto int32Domain() {
    return fuzztest::OneOf(fuzztest::InRange<int32_t>(-1, 1),
                           fuzztest::InRange<int32_t>(-20, 20),
                           fuzztest::Arbitrary<int32_t>(),
                           fuzztest::ElementOf<int32_t>({std::numeric_limits<int32_t>::min(),
                                                         std::numeric_limits<int32_t>::max()}));
}

auto int64Domain() {
    return fuzztest::OneOf(fuzztest::InRange<int64_t>(-1, 1),
                           fuzztest::InRange<int64_t>(-20, 20),
                           fuzztest::Arbitrary<int64_t>(),
                           fuzztest::ElementOf<int64_t>({std::numeric_limits<int64_t>::min(),
                                                         std::numeric_limits<int64_t>::max()}));
}

auto getScalarDomain() {
    return fuzztest::VariantOf(fuzztest::Just(Null{}),
                               fuzztest::Just(Nothing{}),
                               fuzztest::Arbitrary<bool>(),
                               int32Domain(),
                               int64Domain(),
                               fuzztest::Arbitrary<double>());
}

value::TagValueOwned toTagValueOwned(const SBEScalarSpec& spec) {
    return std::visit(
        [](auto&& v) -> value::TagValueOwned {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Null>)
                return value::TagValueOwned::null();
            else if constexpr (std::is_same_v<T, Nothing>)
                return value::TagValueOwned::nothing();
            else if constexpr (std::is_same_v<T, bool>)
                return value::TagValueOwned::boolean(v);
            else if constexpr (std::is_same_v<T, int32_t>)
                return value::TagValueOwned::numberInt32(v);
            else if constexpr (std::is_same_v<T, int64_t>)
                return value::TagValueOwned::numberInt64(v);
            else
                return value::TagValueOwned::numberDouble(v);
        },
        spec);
}

void FuzzScalarCopyRelease(SBEScalarSpec spec) {
    auto tv = toTagValueOwned(spec);
    [[maybe_unused]] auto cp = tv.copy();
}

FUZZ_TEST(SBEValueFuzz, FuzzScalarCopyRelease).WithDomains(getScalarDomain());

void FuzzScalarCompare(SBEScalarSpec specA, SBEScalarSpec specB) {
    auto a = toTagValueOwned(specA);
    auto b = toTagValueOwned(specB);
    value::TagValueOwned cmp(value::compareValue(a.tag(), a.value(), b.tag(), b.value()));
    ASSERT_TRUE(cmp.tag() == value::TypeTags::NumberInt32 || cmp.tag() == value::TypeTags::Nothing);
    if (cmp.tag() == value::TypeTags::Nothing) {
        ASSERT_TRUE(a.tag() == value::TypeTags::Nothing || b.tag() == value::TypeTags::Nothing);
    }
}

FUZZ_TEST(SBEValueFuzz, FuzzScalarCompare).WithDomains(getScalarDomain(), getScalarDomain());

}  // namespace
}  // namespace mongo::sbe

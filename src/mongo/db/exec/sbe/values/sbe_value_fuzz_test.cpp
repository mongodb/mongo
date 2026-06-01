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
                return value::TagValueView::null();
            else if constexpr (std::is_same_v<T, Nothing>)
                return value::TagValueView::nothing();
            else if constexpr (std::is_same_v<T, bool>)
                return value::TagValueView::boolean(v);
            else if constexpr (std::is_same_v<T, int32_t>)
                return value::TagValueView::numberInt32(v);
            else if constexpr (std::is_same_v<T, int64_t>)
                return value::TagValueView::numberInt64(v);
            else
                return value::TagValueView::numberDouble(v);
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

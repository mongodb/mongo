// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/decimal_counter.h"

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace {
using namespace std::literals::string_view_literals;
using namespace mongo;

TEST(DecimalCounter, CountUntilWrapAround) {
    DecimalCounter<uint16_t> counter;
    uint16_t check = 0;
    do {
        std::string_view str = counter;
        ASSERT_EQ(std::to_string(check), str);
        ASSERT_EQ(str.data()[str.size()], '\0');
        ASSERT_EQ(uint16_t(++counter), ++check);
    } while (check);
    ASSERT_EQ(std::string_view(counter), "0"sv);
}
}  // namespace

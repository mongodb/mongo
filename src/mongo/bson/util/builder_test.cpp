/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <limits>

#include "mongo/bson/util/builder.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/str.h"

namespace mongo {
TEST(Builder, String1) {
    const char* big = "eliot was here";
    StringData small(big, 5);
    ASSERT_EQUALS(small, "eliot");

    BufBuilder bb;
    bb.appendStr(small);

    ASSERT_EQUALS(0, strcmp(bb.buf(), "eliot"));
    ASSERT_EQUALS(0, strcmp("eliot", bb.buf()));
}

TEST(Builder, StringBuilderAddress) {
    const void* longPtr = reinterpret_cast<const void*>(-1);
    const void* shortPtr = reinterpret_cast<const void*>(static_cast<uintptr_t>(0xDEADBEEF));

    const void* nullPtr = nullptr;

    StringBuilder sb;
    sb << longPtr;

    if (sizeof(longPtr) == 8) {
        ASSERT_EQUALS("0xFFFFFFFFFFFFFFFF", sb.str());
    } else {
        ASSERT_EQUALS("0xFFFFFFFF", sb.str());
    }

    sb.reset();
    sb << shortPtr;
    ASSERT_EQUALS("0xDEADBEEF", sb.str());

    sb.reset();
    sb << nullPtr;
    ASSERT_EQUALS("0x0", sb.str());
}

TEST(Builder, BooleanOstreamOperator) {
    StringBuilder sb;
    sb << true << false << true;
    ASSERT_EQUALS("101", sb.str());

    sb.reset();
    sb << "{abc: " << true << ", def: " << false << "}";
    ASSERT_EQUALS("{abc: 1, def: 0}", sb.str());
}

TEST(Builder, StackAllocatorShouldNotLeak) {
    StackAllocator<StackSizeDefault> stackAlloc;
    stackAlloc.malloc(StackSizeDefault + 1);  // Force heap allocation.
    // Let the builder go out of scope. If this leaks, it will trip the ASAN leak detector.
}

template <typename T>
void testStringBuilderIntegral() {
    auto check = [](T num) {
        ASSERT_EQ(std::string(str::stream() << num), std::to_string(num));
    };

    // Do some simple sanity checks.
    check(0);
    check(1);
    check(-1);
    check(std::numeric_limits<T>::min());
    check(std::numeric_limits<T>::max());

    // Check the full range of int16_t. Using int32_t as loop variable to detect when we are done.
    for (int32_t num = std::numeric_limits<int16_t>::min();
         num <= std::numeric_limits<int16_t>::max();
         num++) {
        check(num);
    }
}

TEST(Builder, AppendInt) {
    testStringBuilderIntegral<int>();
}
TEST(Builder, AppendUnsigned) {
    testStringBuilderIntegral<unsigned>();
}
TEST(Builder, AppendLong) {
    testStringBuilderIntegral<long>();
}
TEST(Builder, AppendUnsignedLong) {
    testStringBuilderIntegral<unsigned long>();
}
TEST(Builder, AppendLongLong) {
    testStringBuilderIntegral<long long>();
}
TEST(Builder, AppendUnsignedLongLong) {
    testStringBuilderIntegral<unsigned long long>();
}
TEST(Builder, AppendShort) {
    testStringBuilderIntegral<short>();
}
}  // namespace mongo

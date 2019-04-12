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

#include "mongo/platform/basic.h"

#include "mongo/util/concepts.h"

#include <boost/preprocessor/cat.hpp>
#include <cstdint>
#include <string>

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Everything this test tests is tests at compile time. If it compiles, it is a success.
// Intentionally using different metaprogramming techniques as in concepts.h to ensure that the test
// doesn't false negative due to the same compiler bug that would cause problems in the real
// implementation.

template <int num>
using Overload = std::integral_constant<int, num>;

// Using overload resolution in test to ensure a useful error message.
#define ASSERT_SELECTS_OVERLOAD(num, ...) \
    int compileTestName(Overload<num>);   \
    static_assert(sizeof(decltype(compileTestName(__VA_ARGS__))));

// Actual tests are below here. Note to anyone thinking of adding more, You should always have at
// least three overloads with the same signature, since some bugs would only be visible if multiple
// overloads would be disabled.

template <typename T>
struct NonTemplateTest {
    REQUIRES_FOR_NON_TEMPLATE(sizeof(T) == 4)
    static void test() {}

    REQUIRES_FOR_NON_TEMPLATE(sizeof(T) == 8)
    static void test() {}

    REQUIRES_FOR_NON_TEMPLATE(sizeof(T) == 9)
    static void test() {}
};

template <typename T>
constexpr inline auto sizeof_ = sizeof(T);

static_assert(std::is_void_v<decltype(NonTemplateTest<int32_t>::test())>);
static_assert(std::is_void_v<decltype(NonTemplateTest<int64_t>::test())>);
ASSERT_DOES_NOT_COMPILE(typename Char = char, NonTemplateTest<Char>::test());

// Uncomment to see error message.
// auto x = NonTemplateTest<char>::test();

TEMPLATE(typename T)
REQUIRES(sizeof(T) == 0)
Overload<1> requiresTest() {
    return {};
}

TEMPLATE(typename T)
REQUIRES(sizeof(T) == 1)
Overload<2> requiresTest() {
    return {};
}

TEMPLATE(typename T)
REQUIRES(sizeof(T) == 4)
Overload<3> requiresTest() {
    return {};
}

// Note: it is valid to overload template args with typename vs NTTP.
TEMPLATE(int I)
REQUIRES(I == 0)
Overload<11> requiresTest() {
    return {};
}

TEMPLATE(int I)
REQUIRES(I > 0)
Overload<12> requiresTest() {
    return {};
}

TEMPLATE(int I)
REQUIRES(-10 < I && I < 0)
Overload<13> requiresTest();

TEMPLATE(int I)
REQUIRES_OUT_OF_LINE_DEF(-10 < I && I < 0)
Overload<13> requiresTest() {
    return {};
}

ASSERT_SELECTS_OVERLOAD(2, requiresTest<char>());
ASSERT_SELECTS_OVERLOAD(3, requiresTest<int32_t>());
ASSERT_DOES_NOT_COMPILE(typename Int64_t = int64_t, requiresTest<Int64_t>());

ASSERT_SELECTS_OVERLOAD(11, requiresTest<0>());
ASSERT_SELECTS_OVERLOAD(12, requiresTest<1>());
ASSERT_SELECTS_OVERLOAD(13, requiresTest<-1>());
ASSERT_DOES_NOT_COMPILE(int i = -10, requiresTest<i>());

MONGO_MAKE_BOOL_TRAIT(isAddable,
                      (typename LHS, typename RHS),
                      (LHS, RHS),
                      (LHS & mutableLhs, const LHS& lhs, const RHS& rhs),
                      //
                      mutableLhs += rhs,
                      lhs + rhs);

static_assert(isAddable<int, int>);
static_assert(isAddable<int, double>);
static_assert(isAddable<std::string, std::string>);
static_assert(!isAddable<std::string, int>);
static_assert(!isAddable<int, std::string>);


MONGO_MAKE_BOOL_TRAIT(isCallable,
                      (typename Func, typename... Args),
                      (Func, Args...),
                      (Func & func, Args&&... args),
                      //
                      func(args...));

static_assert(isCallable<void()>);
static_assert(!isCallable<void(), int>);
static_assert(isCallable<void(...)>);
static_assert(isCallable<void(...), int>);
static_assert(isCallable<void(...), int, int>);
static_assert(isCallable<void(StringData), StringData>);
static_assert(isCallable<void(StringData), const char*>);
static_assert(isCallable<void(StringData), std::string>);
static_assert(!isCallable<void(StringData)>);
static_assert(!isCallable<void(StringData), int>);
static_assert(!isCallable<void(StringData), StringData, StringData>);

// The unittest framework gets angry if you have no tests.
TEST(Concepts, DummyTest) {}

}  // namespace
}  // namespace mongo

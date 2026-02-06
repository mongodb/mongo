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

/**
 * Unit tests of the unittest framework itself.
 */


#include "mongo/unittest/unittest.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/stringify.h"
#include "mongo/unittest/unittest_main_core.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {
namespace mus = mongo::unittest::stringify;

bool containsPattern(const std::string& pattern, const std::string& value) {
    return value.find(pattern) != std::string::npos;
}

TEST(UnitTestSelfTest, DoNothing) {}

void throwSomething() {
    throw std::exception();
}

TEST(UnitTestSelfTest, TestAssertThrowsSuccess) {
    ASSERT_THROWS(throwSomething(), ::std::exception);
}

class MyException {
public:
    std::string toString() const {
        return what();
    }
    std::string what() const {
        return "whatever";
    }
};

TEST(UnitTestSelfTest, TestAssertThrowsWhatSuccess) {
    ASSERT_THROWS_WHAT(throw MyException(), MyException, "whatever");
}

TEST(UnitTestSelfTest, TestSuccessfulNumericComparisons) {
    ASSERT_EQUALS(1LL, 1.0);
    ASSERT_NOT_EQUALS(1LL, 0.5);
    ASSERT_LESS_THAN(1, 5);
    ASSERT_LESS_THAN_OR_EQUALS(1, 5);
    ASSERT_LESS_THAN_OR_EQUALS(5, 5);
    ASSERT_GREATER_THAN(5, 1);
    ASSERT_GREATER_THAN_OR_EQUALS(5, 1);
    ASSERT_GREATER_THAN_OR_EQUALS(5, 5);
    ASSERT_APPROX_EQUAL(5, 6, 1);
}

TEST(UnitTestSelfTest, BSONObjComparisons) {
    auto a = mongo::BSONObjBuilder{}.append("foo", "bar").obj();
    auto b = mongo::BSONObjBuilder{}.append("foo", "baz").obj();
    ASSERT_BSONOBJ_EQ(a, a);
    ASSERT_BSONOBJ_NE(a, b);
    ASSERT_BSONOBJ_LT(a, b);
    ASSERT_BSONOBJ_LTE(a, b);
    ASSERT_BSONOBJ_LTE(b, b);
    ASSERT_BSONOBJ_GT(b, a);
    ASSERT_BSONOBJ_GTE(b, a);
    ASSERT_BSONOBJ_GTE(a, a);

    ASSERT_BSONOBJ_EQ_UNORDERED(a, a);
    ASSERT_BSONOBJ_NE_UNORDERED(a, b);
    ASSERT_BSONOBJ_LT_UNORDERED(a, b);
    ASSERT_BSONOBJ_LTE_UNORDERED(a, b);
    ASSERT_BSONOBJ_LTE_UNORDERED(b, b);
    ASSERT_BSONOBJ_GT_UNORDERED(b, a);
    ASSERT_BSONOBJ_GTE_UNORDERED(b, a);
    ASSERT_BSONOBJ_GTE_UNORDERED(a, a);
}

TEST(UnitTestSelfTest, BSONObjComparisonsUnordered) {
    auto a = mongo::BSONObjBuilder{}.append("foo", "bar").append("hello", "world").obj();
    auto b = mongo::BSONObjBuilder{}.append("hello", "world").append("foo", "bar").obj();
    ASSERT_BSONOBJ_NE(a, b);
    ASSERT_BSONOBJ_EQ_UNORDERED(a, b);
    ASSERT_BSONOBJ_LTE_UNORDERED(a, b);
    ASSERT_BSONOBJ_LTE_UNORDERED(b, a);
    ASSERT_BSONOBJ_GTE_UNORDERED(b, a);
    ASSERT_BSONOBJ_GTE_UNORDERED(a, b);

    auto c = mongo::BSONObjBuilder{}.append("hello", "world").append("foo", "baz").obj();
    ASSERT_BSONOBJ_NE_UNORDERED(a, c);
    ASSERT_BSONOBJ_NE_UNORDERED(b, c);
    ASSERT_BSONOBJ_LT_UNORDERED(a, c);
    ASSERT_BSONOBJ_LT_UNORDERED(b, c);
    ASSERT_BSONOBJ_GT_UNORDERED(c, b);
    ASSERT_BSONOBJ_GT_UNORDERED(c, a);
}

TEST(UnitTestSelfTest, BSONElementComparisons) {
    auto ao = mongo::BSONObjBuilder{}.append("foo", "bar").obj();
    auto bo = mongo::BSONObjBuilder{}.append("foo", "baz").obj();
    auto a = ao.firstElement();
    auto b = bo.firstElement();
    ASSERT_BSONELT_EQ(a, a);
    ASSERT_BSONELT_NE(a, b);
    ASSERT_BSONELT_LT(a, b);
    ASSERT_BSONELT_LTE(a, a);
    ASSERT_BSONELT_LTE(a, b);
    ASSERT_BSONELT_GT(b, a);
    ASSERT_BSONELT_GTE(b, a);
    ASSERT_BSONELT_GTE(a, a);
}

class UnitTestFormatTest : public mongo::unittest::Test {
public:
    template <template <typename...> class Optional, typename T, typename... As>
    auto mkOptional(As&&... as) {
        return Optional<T>(std::forward<As>(as)...);  // NOLINT
    }

    template <template <typename...> class OptionalTemplate>
    void runFormatOptionalTest() {
        ASSERT_EQ(mus::invoke(mkOptional<OptionalTemplate, int>()), "--");
        ASSERT_EQ(mus::invoke(mkOptional<OptionalTemplate, std::string>()), "--");
        ASSERT_EQ(mus::invoke(mkOptional<OptionalTemplate, int>(123)), " 123");
        ASSERT_EQ(mus::invoke(mkOptional<OptionalTemplate, std::string>("hey")), " hey");
    }

    template <template <typename...> class OptionalTemplate, class None>
    void runEqOptionalTest(None none) {
        ASSERT_EQ(OptionalTemplate<int>{1}, OptionalTemplate<int>{1});  // NOLINT
        ASSERT_NE(OptionalTemplate<int>{1}, OptionalTemplate<int>{2});  // NOLINT
        ASSERT_EQ(OptionalTemplate<int>{}, OptionalTemplate<int>{});    // NOLINT
        ASSERT_EQ(OptionalTemplate<int>{}, none);                       // NOLINT
    }
};

TEST_F(UnitTestFormatTest, FormatBoostOptional) {
    runFormatOptionalTest<boost::optional>();
}

TEST_F(UnitTestFormatTest, EqBoostOptional) {
    runEqOptionalTest<boost::optional>(boost::none);
}

TEST_F(UnitTestFormatTest, FormatStdOptional) {
    runFormatOptionalTest<std::optional>();  // NOLINT
}

TEST_F(UnitTestFormatTest, EqStdOptional) {
    runEqOptionalTest<std::optional>(std::nullopt);  // NOLINT
}

enum class Color { r, g, b };
enum class NamedColor { r, g, b };

inline std::ostream& operator<<(std::ostream& os, const NamedColor& e) {
    return os << std::array{"r", "g", "b"}[static_cast<size_t>(e)];
}

TEST_F(UnitTestFormatTest, FormatEnumClass) {
    ASSERT_STRING_CONTAINS(mus::invoke(Color::r), "Color=0");
    ASSERT_EQ(mus::invoke(NamedColor::r), "r");
    ASSERT_EQ(Color::r, Color::r);
    ASSERT_EQ(NamedColor::r, NamedColor::r);
}

namespace test_extension {
struct X {
    friend std::string stringify_forTest(const X& x) {
        return "X{" + std::to_string(x.x) + "}";
    }

    int x;
};
}  // namespace test_extension

TEST_F(UnitTestFormatTest, FormatCustomized) {
    test_extension::X x{123};
    ASSERT_EQ(mus::invoke(x), "X{123}");
}

DEATH_TEST_REGEX(DeathTestSelfTestDeathTest, TestDeath, "Invariant failure.*false") {
    invariant(false);
}

DEATH_TEST_REGEX(DeathTestSelfTestDeathTest,
                 TestDeathFromException,
                 "noexcept|An exception is active") {
    // throw a non-exception type, just to make sure we catch everything
    struct Dummy {};
    throw Dummy{};
}

class DeathTestSelfTestFixture : public ::mongo::unittest::Test {
public:
    void setUp() override {}
    void tearDown() override {
        LOGV2(24148, "Died in tear-down");
        invariant(false);
    }
};

using DeathTestSelfTestFixtureDeathTest = DeathTestSelfTestFixture;
DEATH_TEST_F(DeathTestSelfTestFixtureDeathTest, DieInTearDown, "Died in tear-down") {}

TEST(UnitTestSelfTest, ComparisonAssertionOverloadResolution) {
    using namespace mongo;

    char xBuf[] = "x";  // Guaranteed different address than "x".
    const char* x = xBuf;

    // At least one StringData, compare contents:
    ASSERT_EQ("x"_sd, "x"_sd);
    ASSERT_EQ("x"_sd, "x");
    ASSERT_EQ("x"_sd, xBuf);
    ASSERT_EQ("x"_sd, x);
    ASSERT_EQ("x", "x"_sd);
    ASSERT_EQ(xBuf, "x"_sd);
    ASSERT_EQ(x, "x"_sd);

    // Otherwise, compare pointers:
    ASSERT_EQ(x, +x);
    ASSERT_EQ(xBuf, +xBuf);
    ASSERT_EQ(x, +xBuf);
    ASSERT_NE("x", +xBuf);
    ASSERT_NE("x", +x);
    ASSERT_NE(+xBuf, "x");
    ASSERT_NE(+x, "x");
}

TEST(UnitTestSelfTest, GtestFilter) {
    using mongo::unittest::gtestFilterForSelection;
    ASSERT_EQ(gtestFilterForSelection({}), "");
    ASSERT_EQ(gtestFilterForSelection({{"A", "X", 0}}), "");
    ASSERT_EQ(gtestFilterForSelection({{"A", "X", 1}}), "A.X");
    ASSERT_EQ(gtestFilterForSelection({{"A", "X", 0}, {"A", "Y", 0}}), "");
    ASSERT_EQ(gtestFilterForSelection({{"A", "X", 1}, {"A", "Y", 1}, {"A", "Z", 1}}),
              "A.X:A.Y:A.Z");
}

class UnitTestPrintingTest : public mongo::unittest::Test {
public:
    static std::string pr(const auto& v) {
        std::ostringstream oss;
        mongo::unittest::universalPrint(v, oss);
        return oss.str();
    }
};

TEST_F(UnitTestPrintingTest, String) {
    using namespace mongo;
    ASSERT_EQ(pr(StringData{"hi"}), "\"hi\"");
    ASSERT_EQ(pr(std::string_view{"hi"}), "\"hi\"");
}

TEST_F(UnitTestPrintingTest, Optional) {
    ASSERT_EQ(pr(boost::none), "(none)");
    ASSERT_EQ(pr(boost::optional<int>(123)), "(123)");
    ASSERT_EQ(pr(boost::optional<std::string>("hi")), "(\"hi\")");

    ASSERT_EQ(pr(std::nullopt), "(nullopt)");
    ASSERT_EQ(pr(std::optional<int>(123)), "(123)");
    ASSERT_EQ(pr(std::optional<std::string>("hi")), "(\"hi\")");
}

TEST_F(UnitTestPrintingTest, Status) {
    using namespace mongo;
    ASSERT_EQ(pr(Status::OK()), "OK");
    ASSERT_EQ(pr(Status{ErrorCodes::UnknownError, ""}), "UnknownError \"\"");
    ASSERT_EQ(pr(Status{ErrorCodes::UnknownError, "reason"}), "UnknownError \"reason\"");
    ASSERT_EQ(pr(StatusWith<int>{ErrorCodes::UnknownError, "reason"}), "UnknownError \"reason\"");
    ASSERT_EQ(pr(StatusWith<int>{123}), "123");
    ASSERT_EQ(pr(StatusWith<std::string>{"hi"}), "\"hi\"");
}

ASSERT_DOES_NOT_COMPILE(DoesNotCompileCheckDeclval, typename Char = char, *std::declval<Char>());
ASSERT_DOES_NOT_COMPILE(DoesNotCompileCheckEnableIf, bool B = false, std::enable_if_t<B, int>{});

// Uncomment to check that it fails when it is supposed to. Unfortunately we can't check in a test
// that this fails when it is supposed to, only that it passes when it should.
//
// ASSERT_DOES_NOT_COMPILE(DoesNotCompileCheckDeclvalFail, typename Char = char,
// *std::declval<Char*>()); ASSERT_DOES_NOT_COMPILE(DoesNotCompileCheckEnableIfFail, bool B = true,
// std::enable_if_t<B, int>{});

class MockNicenessTest : public mongo::unittest::Test {
public:
    using MockBehavior = mongo::unittest::MockBehavior;

    class Actor {
    public:
        virtual ~Actor() = default;
        virtual int action() = 0;
    };

    class MockActor : public Actor {
    public:
        MOCK_METHOD(int, action, (), ());
    };

    static std::unique_ptr<MockActor> makeActor(boost::optional<MockBehavior> behavior = {}) {
        if (behavior) {
            switch (*behavior) {
                case MockBehavior::nice:
                    return std::make_unique<testing::NiceMock<MockActor>>();
                case MockBehavior::naggy:
                    return std::make_unique<testing::NaggyMock<MockActor>>();
                case MockBehavior::strict:
                    return std::make_unique<testing::StrictMock<MockActor>>();
            }
        }
        return std::make_unique<MockActor>();
    }
};

TEST_F(MockNicenessTest, NicenessFlagDefaultValue) {
    ASSERT_EQ(mongo::unittest::getDefaultMockBehavior(), MockBehavior::nice);
}

TEST_F(MockNicenessTest, MockDefaultsToFlagBehavior) {
    setDefaultMockBehavior(MockBehavior::nice);
    ASSERT_EQ(makeActor()->action(), 0);
}

TEST_F(MockNicenessTest, TryFlagsVsMockWrappers) {
    for (auto&& flag : {MockBehavior::nice, MockBehavior::naggy, MockBehavior::strict}) {
        setDefaultMockBehavior(flag);  // Wrappers ignore the default.
        for (auto&& b : {MockBehavior::nice, MockBehavior::naggy})
            ASSERT_EQ(makeActor(b)->action(), 0);
    }
}

}  // namespace

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

#include "mongo/util/functional.h"

#include "mongo/unittest/unittest.h"

/**
 * Note that tests in this file are deliberately outside the mongodb namespace to ensure that
 * deduction works appropriately via adl.  I.e. this set of tests doesn't follow our usual
 * convention, needn't be considered prevailing local style, but should be left alone moving
 * forward.
 */
namespace {

template <typename Sig>
struct FuncObj;
template <typename Ret, typename... Args>
struct FuncObj<Ret(Args...)> {
    Ret operator()(Args...);
};

template <typename Sig>
auto makeFuncObj() -> FuncObj<Sig>;
template <typename Sig>
auto makeFuncPtr() -> Sig*;

#define TEST_DEDUCTION_GUIDE(sig)                                                      \
    static_assert(std::is_same_v<decltype(mongo::unique_function(makeFuncPtr<sig>())), \
                                 mongo::unique_function<sig>>);                        \
    static_assert(std::is_same_v<decltype(mongo::unique_function(makeFuncObj<sig>())), \
                                 mongo::unique_function<sig>>)

TEST_DEDUCTION_GUIDE(void());
TEST_DEDUCTION_GUIDE(void(int));
TEST_DEDUCTION_GUIDE(void(int&));
TEST_DEDUCTION_GUIDE(void(int&&));
TEST_DEDUCTION_GUIDE(void(int, double));
TEST_DEDUCTION_GUIDE(void(int&, double));
TEST_DEDUCTION_GUIDE(void(int&&, double));
TEST_DEDUCTION_GUIDE(int*());
TEST_DEDUCTION_GUIDE(int*(int));
TEST_DEDUCTION_GUIDE(int*(int&));
TEST_DEDUCTION_GUIDE(int*(int&&));
TEST_DEDUCTION_GUIDE(int*(int, double));
TEST_DEDUCTION_GUIDE(int*(int&, double));
TEST_DEDUCTION_GUIDE(int*(int&&, double));

template <int channel>
struct RunDetection {
    ~RunDetection() {
        itRan = false;
    }
    RunDetection(const RunDetection&) = delete;
    RunDetection& operator=(const RunDetection&) = delete;

    RunDetection() {
        itRan = false;
    }

    static bool itRan;
};

template <int channel>
bool RunDetection<channel>::itRan = false;

TEST(UniqueFunctionTest, construct_simple_unique_function_from_lambda) {
    // Implicit construction
    {
        RunDetection<0> runDetection;
        mongo::unique_function<void()> uf = [] { RunDetection<0>::itRan = true; };

        uf();

        ASSERT_TRUE(runDetection.itRan);
    }

    // Explicit construction
    {
        RunDetection<0> runDetection;
        mongo::unique_function<void()> uf{[] { RunDetection<0>::itRan = true; }};

        uf();

        ASSERT_TRUE(runDetection.itRan);
    }
}

TEST(UniqueFunctionTest, assign_simple_unique_function_from_lambda) {
    // Implicit construction
    RunDetection<0> runDetection;
    mongo::unique_function<void()> uf;
    uf = [] { RunDetection<0>::itRan = true; };

    uf();

    ASSERT_TRUE(runDetection.itRan);
}

TEST(UniqueFunctionTest, reassign_simple_unique_function_from_lambda) {
    // Implicit construction
    RunDetection<0> runDetection0;
    RunDetection<1> runDetection1;

    mongo::unique_function<void()> uf = [] { RunDetection<0>::itRan = true; };

    uf = [] { RunDetection<1>::itRan = true; };

    uf();

    ASSERT_FALSE(runDetection0.itRan);
    ASSERT_TRUE(runDetection1.itRan);
}

TEST(UniqueFunctionTest, accepts_a_functor_that_is_move_only) {
    struct Checker {};

    mongo::unique_function<void()> uf = [checkerPtr = std::make_unique<Checker>()]{};

    mongo::unique_function<void()> uf2 = std::move(uf);

    uf = std::move(uf2);
}

TEST(UniqueFunctionTest, dtor_releases_functor_object_and_does_not_call_function) {
    RunDetection<0> runDetection0;
    RunDetection<1> runDetection1;

    struct Checker {
        ~Checker() {
            RunDetection<0>::itRan = true;
        }
    };

    {
        mongo::unique_function<void()> uf = [checkerPtr = std::make_unique<Checker>()] {
            RunDetection<1>::itRan = true;
        };

        ASSERT_FALSE(runDetection0.itRan);
        ASSERT_FALSE(runDetection1.itRan);
    }

    ASSERT_TRUE(runDetection0.itRan);
    ASSERT_FALSE(runDetection1.itRan);
}

TEST(UniqueFunctionTest, comparison_checks) {
    mongo::unique_function<void()> uf;

    // Using true/false assertions, as we're testing the actual operators and commutativity here.
    ASSERT_TRUE(uf == nullptr);
    ASSERT_TRUE(nullptr == uf);
    ASSERT_FALSE(uf != nullptr);
    ASSERT_FALSE(nullptr != uf);

    uf = [] {};

    ASSERT_FALSE(uf == nullptr);
    ASSERT_FALSE(nullptr == uf);
    ASSERT_TRUE(uf != nullptr);
    ASSERT_TRUE(nullptr != uf);

    uf = nullptr;

    ASSERT_TRUE(uf == nullptr);
    ASSERT_TRUE(nullptr == uf);
    ASSERT_FALSE(uf != nullptr);
    ASSERT_FALSE(nullptr != uf);
}

TEST(UniqueFunctionTest, simple_instantiations) {
    mongo::unique_function<void()> a;

    mongo::unique_function<void()> x = []() -> int { return 42; };
    x = []() -> int { return 42; };
}

namespace conversion_checking {
template <typename FT>
using uf = mongo::unique_function<FT>;
template <typename FT>
using sf = std::function<FT>;

// Check expected `is_convertible` traits (which also checks if this kind of conversion will compile
// correctly too.
TEST(UniqueFunctionTest, convertability_tests) {
// TODO when on C++17, see if the new MSVC can handle these `std::isconvertible` static assertions.
#ifndef _MSC_VER
    // Note that `mongo::unique_function` must never convert to `std::function` in any of the
    // following cases.

    // No arguments, return variants

    // Same return type
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void()>, sf<void()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<void()>, uf<void()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<void()>, uf<void()>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int()>, sf<int()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int()>, uf<int()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int()>, uf<int()>>::value);

    // Convertible return type
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int()>, sf<void()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int()>, uf<void()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int()>, uf<void()>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int()>, sf<long()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int()>, uf<long()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int()>, uf<long()>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<const char*()>, sf<std::string()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<const char*()>, uf<std::string()>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<const char*()>, uf<std::string()>>::value);

    // Incompatible return type
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void()>, sf<int()>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void()>, uf<int()>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<void()>, uf<int()>>::value);


    // Argument consistency, with return variants

    // Same return type, same arguments
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(int)>, sf<void(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<void(int)>, uf<void(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<void(int)>, uf<void(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(int)>, sf<int(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int(int)>, uf<int(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int(int)>, uf<int(int)>>::value);

    // Convertible return type, same arguments
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(int)>, sf<void(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int(int)>, uf<void(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int(int)>, uf<void(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(int)>, sf<long(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int(int)>, uf<long(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int(int)>, uf<long(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<const char*(int)>, sf<std::string(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<const char*(int)>, uf<std::string(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<const char*(int)>, uf<std::string(int)>>::value);

    // Incompatible return type, same arguments
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(int)>, sf<int(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(int)>, uf<int(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<void(int)>, uf<int(int)>>::value);


    // Extra arguments, with return variants

    // Same return type, with extra arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void()>, sf<void(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<void()>, uf<void(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void()>, uf<void(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int()>, sf<int(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int()>, uf<int(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<int()>, uf<int(int)>>::value);

    // Convertible return type, with extra arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int()>, sf<void(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int()>, uf<void(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<int()>, uf<void(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int()>, sf<long(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int()>, uf<long(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<int()>, uf<long(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<const char*()>, sf<std::string(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<const char*()>, uf<std::string(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<const char*()>, uf<std::string(int)>>::value);

    // Incompatible return type, with extra arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void()>, sf<int(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void()>, uf<int(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<void()>, uf<int(int)>>::value);


    // Argument conversions, with return variants

    // Same return type, Convertible argument

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(long)>, sf<void(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<void(long)>, uf<void(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<void(long)>, uf<void(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(long)>, sf<int(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int(long)>, uf<int(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int(long)>, uf<int(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(std::string)>, sf<void(const char*)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<void(const char*)>, uf<void(const char*)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<void(std::string)>, uf<void(const char*)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(std::string)>, sf<int(const char*)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int(std::string)>, uf<int(const char*)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int(std::string)>, uf<int(const char*)>>::value);

    // Convertible return type, with convertible arguments
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(long)>, sf<void(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int(long)>, uf<void(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int(long)>, uf<void(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(long)>, sf<long(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int(long)>, uf<long(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int(long)>, uf<long(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<const char*(long)>, sf<std::string(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<const char*(long)>, uf<std::string(int)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<const char*(long)>, uf<std::string(int)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(std::string)>, sf<void(const char*)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int(std::string)>, uf<void(const char*)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int(std::string)>, uf<void(const char*)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(std::string)>, sf<long(const char*)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<uf<int(std::string)>, uf<long(const char*)>>::value);
    MONGO_STATIC_ASSERT(std::is_convertible<sf<int(std::string)>, uf<long(const char*)>>::value);

    MONGO_STATIC_ASSERT(
        !std::is_convertible<uf<const char*(std::string)>, sf<std::string(const char*)>>::value);
    MONGO_STATIC_ASSERT(
        std::is_convertible<uf<const char*(std::string)>, uf<std::string(const char*)>>::value);
    MONGO_STATIC_ASSERT(
        std::is_convertible<sf<const char*(std::string)>, uf<std::string(const char*)>>::value);

    // Incompatible return type, with convertible arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(long)>, sf<int(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(long)>, uf<int(int)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<void(long)>, uf<int(int)>>::value);


    struct X {};
    struct Y {};

    // Incompatible argument conversions, with return variants

    // Same return type
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(X)>, sf<void(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<void(X)>, uf<void(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(X)>, uf<void(Y)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(X)>, sf<int(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(X)>, uf<int(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<int(X)>, uf<int(Y)>>::value);

    // Convertible return type
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(X)>, sf<void(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(X)>, uf<void(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<int(X)>, uf<void(Y)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(X)>, sf<long(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<int(X)>, uf<long(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<int(X)>, uf<long(Y)>>::value);

    MONGO_STATIC_ASSERT(!std::is_convertible<uf<const char*(X)>, sf<std::string(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<const char*(X)>, uf<std::string(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<const char*(X)>, uf<std::string(Y)>>::value);

    // Incompatible return type
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(X)>, sf<int(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<uf<void(X)>, uf<int(Y)>>::value);
    MONGO_STATIC_ASSERT(!std::is_convertible<sf<void(X)>, uf<int(Y)>>::value);
#endif
}
}  // namespace conversion_checking

template <typename U>
bool accept(std::function<void()> arg, U) {
    return false;
}

template <typename T,
          typename U,
          typename = typename std::enable_if<!std::is_convertible<T, std::function<void()>>::value,
                                             void>::type>
bool accept(T arg, U) {
    return true;
}

TEST(UniqueFunctionTest, functionDominanceExample) {
    mongo::unique_function<void()> uf = [] {};

    ASSERT_TRUE(accept(std::move(uf), nullptr));
}

}  // namespace

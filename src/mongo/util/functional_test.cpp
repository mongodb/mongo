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

// These are important to ensure that function_ref is passed in registers in ABIs that we care
// about. There is no way to query this directly, and if there was, we would need to exclude windows
// anyway because it never splits single objects across registers.
static_assert(std::is_trivially_copyable_v<mongo::function_ref<void()>>);
static_assert(sizeof(mongo::function_ref<void()>) == (2 * sizeof(void*)));

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

void markRunDetection0() {
    RunDetection<0>::itRan = true;
}

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

TEST(FunctionRefTest, construct_simple_function_ref_from_lambda) {
    // Implicit construction
    {
        RunDetection<0> runDetection;
        [](mongo::function_ref<void()> fr) {  //
            fr();
        }([] { RunDetection<0>::itRan = true; });

        ASSERT_TRUE(runDetection.itRan);
    }

    // Explicit construction
    {
        RunDetection<0> runDetection;
        mongo::function_ref<void()>([] { RunDetection<0>::itRan = true; })();

        ASSERT_TRUE(runDetection.itRan);
    }
}

TEST(UniqueFunctionTest, works_with_function_name) {
    RunDetection<0> runDetection;
    mongo::unique_function<void()> uf = markRunDetection0;

    uf();

    ASSERT_TRUE(runDetection.itRan);
}

TEST(UniqueFunctionTest, works_with_function_pointer) {
    RunDetection<0> runDetection;
    mongo::unique_function<void()> uf = &markRunDetection0;

    uf();

    ASSERT_TRUE(runDetection.itRan);
}

TEST(FunctionRefTest, works_with_function_name) {
    RunDetection<0> runDetection;
    mongo::function_ref<void()> fr = markRunDetection0;

    fr();

    ASSERT_TRUE(runDetection.itRan);
}

TEST(FunctionRefTest, works_with_function_pointer) {
    RunDetection<0> runDetection;
    mongo::function_ref<void()> fr = &markRunDetection0;

    fr();

    ASSERT_TRUE(runDetection.itRan);
}

TEST(UniqueFunctionTest, returns_value) {
    ASSERT_EQ(mongo::unique_function<int()>([] { return 42; })(), 42);
}

TEST(FunctionRefTest, returns_value) {
    ASSERT_EQ(mongo::function_ref<int()>([] { return 42; })(), 42);
}

TEST(UniqueFunctionTest, takes_arguments) {
    RunDetection<0> runDetection;

    mongo::unique_function<void(int, int)>([](int a, int b) {
        ASSERT_EQ(a, 1);
        ASSERT_EQ(b, 2);
        RunDetection<0>::itRan = true;
    })(1, 2);

    ASSERT_TRUE(runDetection.itRan);
}

TEST(FunctionRefTest, takes_arguments) {
    RunDetection<0> runDetection;

    mongo::function_ref<void(int, int)>([](int a, int b) {
        ASSERT_EQ(a, 1);
        ASSERT_EQ(b, 2);
        RunDetection<0>::itRan = true;
    })(1, 2);

    ASSERT_TRUE(runDetection.itRan);
}

TEST(UniqueFunctionTest, returns_reference) {
    struct Immobile {
        Immobile() = default;
        Immobile(Immobile&&) = delete;
    };

    Immobile object;
    ASSERT_EQ(&(mongo::unique_function<Immobile&()>([&]() -> Immobile& { return object; })()),
              &object);
}

TEST(FunctionRefTest, returns_reference) {
    struct Immobile {
        Immobile() = default;
        Immobile(Immobile&&) = delete;
    };

    Immobile object;
    ASSERT_EQ(&(mongo::function_ref<Immobile&()>([&]() -> Immobile& { return object; })()),
              &object);
}

TEST(UniqueFunctionTest, assign_simple_unique_function_from_lambda) {
    // Implicit construction
    RunDetection<0> runDetection;
    mongo::unique_function<void()> uf;
    uf = [] { RunDetection<0>::itRan = true; };

    uf();

    ASSERT_TRUE(runDetection.itRan);
}
static_assert(!std::is_default_constructible_v<mongo::function_ref<void()>>);
static_assert(std::is_nothrow_move_assignable_v<mongo::function_ref<void()>>);
static_assert(std::is_nothrow_copy_assignable_v<mongo::function_ref<void()>>);
static_assert(std::is_assignable_v<mongo::function_ref<void()>, void (*)()>);
static_assert(std::is_assignable_v<mongo::function_ref<void()>, void (&)()>);
static_assert(!std::is_assignable_v<mongo::function_ref<void()>, std::function<void()>>);

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

TEST(FunctionRefTest, reassign_simple_function_ref_from_decayed_lambda) {
    // Implicit construction
    RunDetection<0> runDetection;
    mongo::function_ref<void()> fr = +[] {};
    fr = +[] { RunDetection<0>::itRan = true; };

    fr();

    ASSERT_TRUE(runDetection.itRan);
}


TEST(FunctionRefTest, reassign_simple_function_ref_from_function_ref) {
    // Implicit construction
    RunDetection<0> runDetection0;
    RunDetection<1> runDetection1;

    [](mongo::function_ref<void()> fr0, mongo::function_ref<void()> fr1) {
        fr0 = fr1;
        fr0();
    }([] { RunDetection<0>::itRan = true; }, [] { RunDetection<1>::itRan = true; });

    ASSERT_FALSE(runDetection0.itRan);
    ASSERT_TRUE(runDetection1.itRan);
}

TEST(UniqueFunctionTest, accepts_a_functor_that_is_move_only) {
    struct Checker {};

    mongo::unique_function<void()> uf = [checkerPtr = std::make_unique<Checker>()] {};

    mongo::unique_function<void()> uf2 = std::move(uf);

    uf = std::move(uf2);
}

TEST(FunctionRefTest, accepts_a_functor_that_is_immobile) {
    struct Immobile {
        Immobile() = default;
        Immobile(Immobile&&) = delete;

        void operator()() {
            RunDetection<0>::itRan = true;
        }
    };

    {
        RunDetection<0> runDetection0;

        [](mongo::function_ref<void()> func) {  //
            func();
        }(Immobile());

        ASSERT_TRUE(runDetection0.itRan);
    }
    {
        RunDetection<0> runDetection0;

        Immobile immobile;
        [](mongo::function_ref<void()> func) {  //
            func();
        }(immobile);

        ASSERT_TRUE(runDetection0.itRan);
    }
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

TEST(FunctionRefTest, simple_instantiations) {
    mongo::function_ref<void()>([]() -> int { return 42; });
}

namespace conversion_checking {
template <typename FT>
using fr = mongo::function_ref<FT>;
template <typename FT>
using uf = mongo::unique_function<FT>;
template <typename FT>
using sf = std::function<FT>;

// Check expected `is_convertible_v` traits (which also checks if this kind of conversion will
// compile correctly too.
TEST(UniqueFunctionTest, convertibility_tests) {
// TODO when on C++17, see if the new MSVC can handle these `std::isconvertible` static assertions.
#ifndef _MSC_VER
    // Note that `mongo::unique_function` must never convert to `std::function` in any of the
    // following cases.

    // No arguments, return variants

    // Same return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void()>, sf<void()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<void()>, uf<void()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<void()>, uf<void()>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int()>, sf<int()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int()>, uf<int()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int()>, uf<int()>>);

    // Convertible return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int()>, sf<void()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int()>, uf<void()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int()>, uf<void()>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int()>, sf<long()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int()>, uf<long()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int()>, uf<long()>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<const char*()>, sf<std::string()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<const char*()>, uf<std::string()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<const char*()>, uf<std::string()>>);

    // Incompatible return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void()>, sf<int()>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void()>, uf<int()>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void()>, uf<int()>>);


    // Argument consistency, with return variants

    // Same return type, same arguments
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(int)>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<void(int)>, uf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<void(int)>, uf<void(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(int)>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int(int)>, uf<int(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(int)>, uf<int(int)>>);

    // Convertible return type, same arguments
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(int)>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int(int)>, uf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(int)>, uf<void(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(int)>, sf<long(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int(int)>, uf<long(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(int)>, uf<long(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<const char*(int)>, sf<std::string(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<const char*(int)>, uf<std::string(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<const char*(int)>, uf<std::string(int)>>);

    // Incompatible return type, same arguments
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(int)>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(int)>, uf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void(int)>, uf<int(int)>>);


    // Extra arguments, with return variants

    // Same return type, with extra arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void()>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void()>, uf<void(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void()>, uf<void(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int()>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int()>, uf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int()>, uf<int(int)>>);

    // Convertible return type, with extra arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int()>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int()>, uf<void(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int()>, uf<void(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int()>, sf<long(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int()>, uf<long(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int()>, uf<long(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<const char*()>, sf<std::string(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<const char*()>, uf<std::string(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<const char*()>, uf<std::string(int)>>);

    // Incompatible return type, with extra arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void()>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void()>, uf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void()>, uf<int(int)>>);


    // Argument conversions, with return variants

    // Same return type, Convertible argument

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(long)>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<void(long)>, uf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<void(long)>, uf<void(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(long)>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int(long)>, uf<int(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(long)>, uf<int(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(std::string)>, sf<void(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<void(const char*)>, uf<void(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<void(std::string)>, uf<void(const char*)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(std::string)>, sf<int(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int(std::string)>, uf<int(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(std::string)>, uf<int(const char*)>>);

    // Convertible return type, with convertible arguments
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(long)>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int(long)>, uf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(long)>, uf<void(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(long)>, sf<long(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int(long)>, uf<long(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(long)>, uf<long(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<const char*(long)>, sf<std::string(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<const char*(long)>, uf<std::string(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<const char*(long)>, uf<std::string(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(std::string)>, sf<void(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int(std::string)>, uf<void(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(std::string)>, uf<void(const char*)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(std::string)>, sf<long(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<uf<int(std::string)>, uf<long(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(std::string)>, uf<long(const char*)>>);

    MONGO_STATIC_ASSERT(
        !std::is_convertible_v<uf<const char*(std::string)>, sf<std::string(const char*)>>);
    MONGO_STATIC_ASSERT(
        std::is_convertible_v<uf<const char*(std::string)>, uf<std::string(const char*)>>);
    MONGO_STATIC_ASSERT(
        std::is_convertible_v<sf<const char*(std::string)>, uf<std::string(const char*)>>);

    // Incompatible return type, with convertible arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(long)>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(long)>, uf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void(long)>, uf<int(int)>>);


    struct X {};
    struct Y {};

    // Incompatible argument conversions, with return variants

    // Same return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(X)>, sf<void(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void(X)>, uf<void(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(X)>, uf<void(Y)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(X)>, sf<int(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(X)>, uf<int(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int(X)>, uf<int(Y)>>);

    // Convertible return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(X)>, sf<void(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(X)>, uf<void(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int(X)>, uf<void(Y)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(X)>, sf<long(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<int(X)>, uf<long(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int(X)>, uf<long(Y)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<const char*(X)>, sf<std::string(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<const char*(X)>, uf<std::string(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<const char*(X)>, uf<std::string(Y)>>);

    // Incompatible return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(X)>, sf<int(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<uf<void(X)>, uf<int(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void(X)>, uf<int(Y)>>);
#endif
}

// function_ref is convertible to and from a std::function with a compatible signature.
TEST(FunctionRefTest, convertibility_tests) {
    // No arguments, return variants

    // Same return type
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<void()>, sf<void()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<void()>, fr<void()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<void()>, fr<void()>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int()>, sf<int()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int()>, fr<int()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int()>, fr<int()>>);

    // Convertible return type
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int()>, sf<void()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int()>, fr<void()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int()>, fr<void()>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int()>, sf<long()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int()>, fr<long()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int()>, fr<long()>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<const char*()>, sf<std::string()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<const char*()>, fr<std::string()>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<const char*()>, fr<std::string()>>);

    // Incompatible return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void()>, sf<int()>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void()>, fr<int()>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void()>, fr<int()>>);


    // Argument consistency, with return variants

    // Same return type, same arguments
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<void(int)>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<void(int)>, fr<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<void(int)>, fr<void(int)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(int)>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(int)>, fr<int(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(int)>, fr<int(int)>>);

    // Convertible return type, same arguments
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(int)>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(int)>, fr<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(int)>, fr<void(int)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(int)>, sf<long(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(int)>, fr<long(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(int)>, fr<long(int)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<const char*(int)>, sf<std::string(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<const char*(int)>, fr<std::string(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<const char*(int)>, fr<std::string(int)>>);

    // Incompatible return type, same arguments
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void(int)>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void(int)>, fr<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void(int)>, fr<int(int)>>);


    // Extra arguments, with return variants

    // Same return type, with extra arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void()>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void()>, fr<void(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void()>, fr<void(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int()>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int()>, fr<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int()>, fr<int(int)>>);

    // Convertible return type, with extra arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int()>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int()>, fr<void(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int()>, fr<void(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int()>, sf<long(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int()>, fr<long(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int()>, fr<long(int)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<const char*()>, sf<std::string(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<const char*()>, fr<std::string(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<const char*()>, fr<std::string(int)>>);

    // Incompatible return type, with extra arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void()>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void()>, fr<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void()>, fr<int(int)>>);


    // Argument conversions, with return variants

    // Same return type, Convertible argument

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<void(long)>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<void(long)>, fr<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<void(long)>, fr<void(int)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(long)>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(long)>, fr<int(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(long)>, fr<int(int)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<void(std::string)>, sf<void(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<void(const char*)>, fr<void(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<void(std::string)>, fr<void(const char*)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(std::string)>, sf<int(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(std::string)>, fr<int(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(std::string)>, fr<int(const char*)>>);

    // Convertible return type, with convertible arguments
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(long)>, sf<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(long)>, fr<void(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(long)>, fr<void(int)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(long)>, sf<long(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(long)>, fr<long(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(long)>, fr<long(int)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<const char*(long)>, sf<std::string(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<const char*(long)>, fr<std::string(int)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<const char*(long)>, fr<std::string(int)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(std::string)>, sf<void(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(std::string)>, fr<void(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(std::string)>, fr<void(const char*)>>);

    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(std::string)>, sf<long(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<fr<int(std::string)>, fr<long(const char*)>>);
    MONGO_STATIC_ASSERT(std::is_convertible_v<sf<int(std::string)>, fr<long(const char*)>>);

    MONGO_STATIC_ASSERT(
        std::is_convertible_v<fr<const char*(std::string)>, sf<std::string(const char*)>>);
    MONGO_STATIC_ASSERT(
        std::is_convertible_v<fr<const char*(std::string)>, fr<std::string(const char*)>>);
    MONGO_STATIC_ASSERT(
        std::is_convertible_v<sf<const char*(std::string)>, fr<std::string(const char*)>>);

    // Incompatible return type, with convertible arguments (Not permitted)
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void(long)>, sf<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void(long)>, fr<int(int)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void(long)>, fr<int(int)>>);


    struct X {};
    struct Y {};

    // Incompatible argument conversions, with return variants

    // Same return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void(X)>, sf<void(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void(X)>, fr<void(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void(X)>, fr<void(Y)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int(X)>, sf<int(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int(X)>, fr<int(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int(X)>, fr<int(Y)>>);

    // Convertible return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int(X)>, sf<void(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int(X)>, fr<void(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int(X)>, fr<void(Y)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int(X)>, sf<long(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<int(X)>, fr<long(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<int(X)>, fr<long(Y)>>);

    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<const char*(X)>, sf<std::string(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<const char*(X)>, fr<std::string(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<const char*(X)>, fr<std::string(Y)>>);

    // Incompatible return type
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void(X)>, sf<int(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<fr<void(X)>, fr<int(Y)>>);
    MONGO_STATIC_ASSERT(!std::is_convertible_v<sf<void(X)>, fr<int(Y)>>);
}
}  // namespace conversion_checking

template <typename U>
bool accept(std::function<void()> arg, U) {
    return false;
}

template <typename T,
          typename U,
          typename = std::enable_if_t<!std::is_convertible_v<T, std::function<void()>>, void>>
bool accept(T arg, U) {
    return true;
}

TEST(UniqueFunctionTest, functionDominanceExample) {
    mongo::unique_function<void()> uf = [] {};

    ASSERT_TRUE(accept(std::move(uf), nullptr));
}

// Enable these tests to manually verify that we get warnings (which are promoted to errors).
// Note: because the warning is from inside the template instantiations, it usually won't show up
// with clangd, you need to do an actual build.
#if 0
TEST(UniqueFunctionTest, WarnWhenIgnoringStatus) {
    mongo::unique_function<void()>([] { return mongo::Status::OK(); })();
}
TEST(FunctionRefTest, WarnWhenIgnoringStatus) {
    mongo::function_ref<void()>([] { return mongo::Status::OK(); })();
}
#endif

}  // namespace

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


#include <type_traits>

#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

namespace {

static int numConstructedAs;
static int numCopyConstructedAs;
static int numCopyAssignedAs;
static int numDestructedAs;

class A {
public:
    A() : value(0) {
        ++numConstructedAs;
    }
    A(const A& other) : value(other.value) {
        ++numCopyConstructedAs;
    }
    A& operator=(const A& rhs) {
        value = rhs.value;
        ++numCopyAssignedAs;
        return *this;
    }
    ~A() {
        ++numDestructedAs;
    }
    int value;
};

class ThrowA {
public:
    ThrowA() : value(0) {
        uasserted(ErrorCodes::Unauthorized, "Throwing in a constructor");
    }

    int value;
};

struct NonCopyableA {
    A a;
    NonCopyableA() = default;
    NonCopyableA(const NonCopyableA&) = delete;
    NonCopyableA& operator=(const NonCopyableA&) = delete;
};

TEST(DecorableTest, SimpleDecoration) {
    struct MyDecorable : Decorable<MyDecorable> {};
    numConstructedAs = 0;
    numDestructedAs = 0;
    static const auto dd1 = MyDecorable::template declareDecoration<A>();
    static const auto dd2 = MyDecorable::template declareDecoration<A>();
    static const auto dd3 = MyDecorable::template declareDecoration<int>();

    {
        MyDecorable decorable1;
        ASSERT_EQ(2, numConstructedAs);
        ASSERT_EQ(0, numDestructedAs);
        MyDecorable decorable2;
        ASSERT_EQ(4, numConstructedAs);
        ASSERT_EQ(0, numDestructedAs);

        ASSERT_EQ(0, dd1(decorable1).value);
        ASSERT_EQ(0, dd2(decorable1).value);
        ASSERT_EQ(0, dd1(decorable2).value);
        ASSERT_EQ(0, dd2(decorable2).value);
        ASSERT_EQ(0, dd3(decorable2));
        dd1(decorable1).value = 1;
        dd2(decorable1).value = 2;
        dd1(decorable2).value = 3;
        dd2(decorable2).value = 4;
        dd3(decorable2) = 5;
        ASSERT_EQ(1, dd1(decorable1).value);
        ASSERT_EQ(2, dd2(decorable1).value);
        ASSERT_EQ(3, dd1(decorable2).value);
        ASSERT_EQ(4, dd2(decorable2).value);
        ASSERT_EQ(5, dd3(decorable2));
    }
    ASSERT_EQ(4, numDestructedAs);
}

TEST(DecorableTest, ThrowingConstructor) {
    struct MyDecorable : Decorable<MyDecorable> {};
    numConstructedAs = 0;
    numDestructedAs = 0;
    static const auto dd1 [[maybe_unused]] = MyDecorable::template declareDecoration<A>();
    static const auto dd2 [[maybe_unused]] = MyDecorable::template declareDecoration<ThrowA>();
    static const auto dd3 [[maybe_unused]] = MyDecorable::template declareDecoration<A>();

    try {
        MyDecorable decorable;
        FAIL("didn't throw");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ErrorCodes::Unauthorized, ex.code());
    }
    ASSERT_EQ(1, numConstructedAs);
    ASSERT_EQ(1, numDestructedAs);
}

TEST(DecorableTest, Alignment) {
    struct MyDecorable : Decorable<MyDecorable> {};
    static const auto firstChar [[maybe_unused]] = MyDecorable::template declareDecoration<char>();
    static const auto firstInt = MyDecorable::template declareDecoration<int>();
    static const auto secondChar [[maybe_unused]] = MyDecorable::template declareDecoration<char>();
    static const auto secondInt = MyDecorable::template declareDecoration<int>();

    MyDecorable d;
    ASSERT_EQ(0U, reinterpret_cast<uintptr_t>(&firstInt(d)) % alignof(int));
    ASSERT_EQ(0U, reinterpret_cast<uintptr_t>(&secondInt(d)) % alignof(int));
}

struct DecoratedOwnerChecker : public Decorable<DecoratedOwnerChecker> {
    const char answer[100] = "The answer to life the universe and everything is 42";
};

// Test all 4 variations of the owner back reference: const pointer, non-const pointer, const
// reference, non-const reference.
struct DecorationWithOwner {
    DecorationWithOwner() {}

    static const DecoratedOwnerChecker::Decoration<DecorationWithOwner> get;

    std::string getTheAnswer1() const {
        // const pointer variant
        auto* const owner = get.owner(this);
        static_assert(std::is_same<const DecoratedOwnerChecker* const, decltype(owner)>::value,
                      "Type of fetched owner pointer is incorrect.");
        return owner->answer;
    }

    std::string getTheAnswer2() {
        // non-const pointer variant
        DecoratedOwnerChecker* const owner = get.owner(this);
        return owner->answer;
    }

    std::string getTheAnswer3() const {
        // const reference variant
        auto& owner = get.owner(*this);
        static_assert(std::is_same<const DecoratedOwnerChecker&, decltype(owner)>::value,
                      "Type of fetched owner reference is incorrect.");
        return owner.answer;
    }

    std::string getTheAnswer4() {
        // Non-const reference variant
        DecoratedOwnerChecker& owner = get.owner(*this);
        return owner.answer;
    }
};

const DecoratedOwnerChecker::Decoration<DecorationWithOwner> DecorationWithOwner::get =
    DecoratedOwnerChecker::declareDecoration<DecorationWithOwner>();


TEST(DecorableTest, DecorationWithOwner) {
    DecoratedOwnerChecker owner;
    const std::string answer = owner.answer;
    ASSERT_NE(answer, "");

    const std::string witness1 = DecorationWithOwner::get(owner).getTheAnswer1();
    ASSERT_EQ(answer, witness1);

    const std::string witness2 = DecorationWithOwner::get(owner).getTheAnswer2();
    ASSERT_EQ(answer, witness2);

    const std::string witness3 = DecorationWithOwner::get(owner).getTheAnswer3();
    ASSERT_EQ(answer, witness3);

    const std::string witness4 = DecorationWithOwner::get(owner).getTheAnswer4();
    ASSERT_EQ(answer, witness4);

    DecorationWithOwner& decoration = DecorationWithOwner::get(owner);
    ASSERT_EQ(&owner, &DecorationWithOwner::get.owner(decoration));
}

TEST(DecorableTest, NonCopyableDecorable) {
    struct MyDecorable : Decorable<MyDecorable> {
        MyDecorable() = default;
        MyDecorable(const MyDecorable&) = delete;
        MyDecorable& operator=(const MyDecorable&) = delete;
    };

    numCopyConstructedAs = 0;
    numCopyAssignedAs = 0;
    static const auto dd1 = MyDecorable::declareDecoration<NonCopyableA>();
    MyDecorable decorable;
    dd1(decorable).a.value = 1;
}

TEST(DecorableTest, CopyableDecorable) {
    struct MyCopyableDecorable : Decorable<MyCopyableDecorable> {};
    numCopyConstructedAs = 0;
    numCopyAssignedAs = 0;
    static const auto dd1 = MyCopyableDecorable::declareDecoration<A>();
    static const auto dd2 = MyCopyableDecorable::declareDecoration<int>();

    {
        MyCopyableDecorable decorable1;
        dd1(decorable1).value = 1;
        dd2(decorable1) = 2;

        MyCopyableDecorable decorable2(decorable1);
        ASSERT_EQ(1, numCopyConstructedAs);
        ASSERT_EQ(0, numCopyAssignedAs);
        ASSERT_EQ(dd1(decorable1).value, dd1(decorable2).value);
        ASSERT_EQ(dd2(decorable1), dd2(decorable2));

        MyCopyableDecorable decorable3;
        ASSERT_NE(dd1(decorable1).value, dd1(decorable3).value);
        ASSERT_NE(dd2(decorable1), dd2(decorable3));

        decorable3 = decorable1;
        ASSERT_EQ(1, numCopyConstructedAs);
        ASSERT_EQ(1, numCopyAssignedAs);
        ASSERT_EQ(dd1(decorable1).value, dd1(decorable3).value);
        ASSERT_EQ(dd2(decorable1), dd2(decorable3));
    }
}

}  // namespace
}  // namespace mongo

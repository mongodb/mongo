/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <fmt/format.h>

#include "mongo/util/packaged_task.h"

#include "mongo/base/error_codes.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

namespace mongo {
namespace {
using namespace fmt::literals;

TEST(PackagedTaskTest, LambdaTaskNoArgs) {
    auto packagedGetBSON = PackagedTask([] { return BSON("x" << 42); });
    auto bsonFut = packagedGetBSON.getFuture();
    ASSERT_FALSE(bsonFut.isReady());
    packagedGetBSON();
    ASSERT_BSONOBJ_EQ(bsonFut.get(), BSON("x" << 42));
}

TEST(PackagedTaskTest, LambdaTaskOneArg) {
    auto packagedHelloName =
        PackagedTask([](std::string name) { return "Hello, {}!"_format(name); });
    auto helloFuture = packagedHelloName.getFuture();
    ASSERT_FALSE(helloFuture.isReady());
    packagedHelloName("George");
    ASSERT_EQ(std::move(helloFuture).get(), "Hello, George!");
}

TEST(PackagedTaskTest, LambdaTaskMultipleArgs) {
    auto packagedPrintNameAndAge = PackagedTask(
        [](std::string name, int age) { return "{} is {} years old!"_format(name, age); });
    auto nameAndAgeFut = packagedPrintNameAndAge.getFuture();
    ASSERT_FALSE(nameAndAgeFut.isReady());
    packagedPrintNameAndAge("George", 24);
    ASSERT_EQ(std::move(nameAndAgeFut).get(), "George is 24 years old!");
}

TEST(PackagedTaskTest, UniqueFunctionTaskNoArgs) {
    unique_function<BSONObj()> fn = [] { return BSON("x" << 42); };
    auto packagedGetBSON = PackagedTask(std::move(fn));
    auto bsonFut = packagedGetBSON.getFuture();
    ASSERT_FALSE(bsonFut.isReady());
    packagedGetBSON();
    ASSERT_BSONOBJ_EQ(bsonFut.get(), BSON("x" << 42));
}

TEST(PackagedTaskTest, UniqueFunctionTaskOneArg) {
    unique_function<std::string(std::string)> fn = [](std::string name) {
        return "Hello, {}!"_format(name);
    };
    auto packagedHelloName = PackagedTask(std::move(fn));
    auto helloFuture = packagedHelloName.getFuture();
    ASSERT_FALSE(helloFuture.isReady());
    packagedHelloName("George");
    ASSERT_EQ(std::move(helloFuture).get(), "Hello, George!");
}

TEST(PackagedTaskTest, UniqueFunctionTaskMultipleArgs) {
    unique_function<std::string(std::string, int)> fn = [](std::string name, int age) {
        return "{} is {} years old!"_format(name, age);
    };
    auto packagedPrintNameAndAge = PackagedTask(std::move(fn));
    auto nameAndAgeFut = packagedPrintNameAndAge.getFuture();
    ASSERT_FALSE(nameAndAgeFut.isReady());
    packagedPrintNameAndAge("George", 24);
    ASSERT_EQ(std::move(nameAndAgeFut).get(), "George is 24 years old!");
}

TEST(PackagedTaskTest, FunctionPointerTaskNoArgs) {
    auto getNumPackagedTask = PackagedTask(+[] { return 42; });
    auto getNumFut = getNumPackagedTask.getFuture();
    ASSERT_FALSE(getNumFut.isReady());
    getNumPackagedTask();
    ASSERT_EQ(std::move(getNumFut).get(), 42);
}

TEST(PackagedTaskTest, FunctionPointerTaskOneArg) {
    auto getNumPlusNPackagedTask = PackagedTask(+[](int n) { return 42 + n; });
    auto getNumPlusNFut = getNumPlusNPackagedTask.getFuture();
    ASSERT_FALSE(getNumPlusNFut.isReady());
    getNumPlusNPackagedTask(2);
    ASSERT_EQ(std::move(getNumPlusNFut).get(), 44);
}

TEST(PackagedTaskTest, FunctionPointerTaskMultipleArgs) {
    auto getSumPackagedTask = PackagedTask(+[](int x, int y) { return x + y; });
    auto getSumFut = getSumPackagedTask.getFuture();
    ASSERT_FALSE(getSumFut.isReady());
    getSumPackagedTask(2, 6);
    ASSERT_EQ(std::move(getSumFut).get(), 8);
}

TEST(PackagedTaskTest, FutureReturningTaskNoArgument) {
    auto [p, f] = makePromiseFuture<std::string>();
    auto greeting = PackagedTask([greetWordFut = std::move(f)]() mutable {
        return std::move(greetWordFut).then([](std::string greetWord) {
            return "{}George"_format(greetWord);
        });
    });
    auto greetingFut = greeting.getFuture();
    ASSERT_FALSE(greetingFut.isReady());
    greeting();
    ASSERT_FALSE(greetingFut.isReady());
    p.emplaceValue("Aloha, ");
    ASSERT_EQ(std::move(greetingFut).get(), "Aloha, George");
}

TEST(PackagedTaskTest, FutureReturningTaskWithArgument) {
    auto [p, f] = makePromiseFuture<std::string>();
    auto greetingWithName = PackagedTask([greetingFut = std::move(f)](std::string name) mutable {
        return std::move(greetingFut).then([name = std::move(name)](std::string greeting) {
            return "{}{}"_format(greeting, name);
        });
    });
    auto greetingWithNameFut = greetingWithName.getFuture();
    ASSERT_FALSE(greetingWithNameFut.isReady());
    greetingWithName("George");
    ASSERT_FALSE(greetingWithNameFut.isReady());
    p.emplaceValue("Aloha, ");
    ASSERT_EQ(std::move(greetingWithNameFut).get(), "Aloha, George");
}

TEST(PackagedTaskTest, CanOnlyExtractOneFuture) {
    auto packagedHelloWorld = PackagedTask([] { return "Hello, World!"; });
    auto future = packagedHelloWorld.getFuture();
    ASSERT_THROWS_CODE(
        packagedHelloWorld.getFuture(), DBException, ErrorCodes::FutureAlreadyRetrieved);
}

TEST(PackagedTaskTest, BreaksPromiseIfNeverRun) {
    Future<const char*> fut = [&] {
        auto packagedHelloWorld = PackagedTask([] { return "Hello, World!"; });
        auto fut = packagedHelloWorld.getFuture();
        ASSERT_FALSE(fut.isReady());
        return fut;
    }();
    ASSERT_THROWS_CODE(fut.get(), DBException, ErrorCodes::BrokenPromise);
}
}  // namespace
}  // namespace mongo

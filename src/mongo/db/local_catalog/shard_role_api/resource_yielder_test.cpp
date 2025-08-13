/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class NoopResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext*) override {}
    void unyield(OperationContext*) override {}
};

TEST(ResourceYielderTest, RunWithYieldingSuccess) {
    auto noopYielder = std::make_unique<NoopResourceYielder>();
    auto result = runWithYielding(nullptr, noopYielder.get(), [] { return 5; });
    ASSERT_EQ(result, 5);
}

TEST(ResourceYielderTest, RunWithYieldingRunFails) {
    auto noopYielder = std::make_unique<NoopResourceYielder>();
    ASSERT_THROWS_CODE(runWithYielding(nullptr,
                                       noopYielder.get(),
                                       [] {
                                           uasserted(ErrorCodes::InternalError, "Callback error");
                                           return 5;
                                       }),
                       DBException,
                       ErrorCodes::InternalError);
}

class ThrowyYieldResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext*) override {
        uasserted(ErrorCodes::ConflictingOperationInProgress, "Yield error");
    }

    void unyield(OperationContext*) override {}
};

TEST(ResourceYielderTest, RunWithYieldingYieldFails) {
    auto throwyYielder = std::make_unique<ThrowyYieldResourceYielder>();

    // Failure to yield returns error even if callback did not error.
    ASSERT_THROWS_CODE(runWithYielding(nullptr, throwyYielder.get(), [] { return 5; }),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    // Failure to yield returns its error over an error thrown by the callback, which shouldn't run.
    bool callbackRan = false;
    ASSERT_THROWS_CODE(runWithYielding(nullptr,
                                       throwyYielder.get(),
                                       [&] {
                                           callbackRan = true;
                                           uasserted(ErrorCodes::InternalError, "Callback error");
                                           return 5;
                                       }),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
    ASSERT_FALSE(callbackRan);  // Callback never ran.
}

class ThrowyUnyieldResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext*) override {}

    void unyield(OperationContext*) override {
        uasserted(ErrorCodes::BadValue, "Unyield error");
    }
};

TEST(ResourceYielderTest, RunWithYieldingUnyieldFails) {
    auto throwyUnyielder = std::make_unique<ThrowyUnyieldResourceYielder>();

    // Failure to unyield returns error even if callback did not error.
    ASSERT_THROWS_CODE(runWithYielding(nullptr, throwyUnyielder.get(), [] { return 5; }),
                       DBException,
                       ErrorCodes::BadValue);

    // Failure to unyield returns its error over an error thrown by the callback
    bool callbackRan = false;
    ASSERT_THROWS_CODE(runWithYielding(nullptr,
                                       throwyUnyielder.get(),
                                       [&] {
                                           callbackRan = true;
                                           uasserted(ErrorCodes::InternalError, "Callback error");
                                           return 5;
                                       }),
                       DBException,
                       ErrorCodes::BadValue);
    ASSERT(callbackRan);  // Callback still ran.
}

class ThrowyYieldAndUnyieldResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext*) override {
        uasserted(ErrorCodes::ConflictingOperationInProgress, "Yield error");
    }

    void unyield(OperationContext*) override {
        ranUnyield = true;
        uasserted(ErrorCodes::BadValue, "Unyield error");
    }

    bool ranUnyield = false;
};

TEST(ResourceYielderTest, RunWithYieldingBothYieldAndUnyieldFail) {
    auto bothThrowyYielder = std::make_unique<ThrowyYieldAndUnyieldResourceYielder>();

    // Failure to yield should prevent unyielding and the callback from running.
    bool callbackRan = false;
    ASSERT_THROWS_CODE(runWithYielding(nullptr,
                                       bothThrowyYielder.get(),
                                       [&] {
                                           callbackRan = true;
                                           return 5;
                                       }),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
    ASSERT_FALSE(callbackRan);                    // Callback never ran.
    ASSERT_FALSE(bothThrowyYielder->ranUnyield);  // Unyield never ran.
}

}  // unnamed namespace
}  // namespace mongo

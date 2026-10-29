// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/resource_yielder.h"

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

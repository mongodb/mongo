/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/primary_only_service_helpers/with_automatic_retry.h"

#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"

#include <memory>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace primary_only_service_helpers {

class WithAutomaticRetryTest : public unittest::Test {
protected:
    void setUp() override {
        _executor = std::make_shared<ThreadPool>([]() {
            ThreadPool::Options options;
            options.maxThreads = 2;
            return options;
        }());
        _executor->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
    }

    std::shared_ptr<ThreadPool> getExecutor() const {
        return _executor;
    }

    std::vector<Status> getDefaultRetryableErrors() {
        return {Status(ErrorCodes::Interrupted, "foo"),
                Status(ErrorCodes::PrimarySteppedDown, "foo"),
                Status(ErrorCodes::PeriodicJobIsStopped, "foo"),
                Status(ErrorCodes::FailedToSatisfyReadPreference, "foo"),
                Status(ErrorCodes::ShardingStateNotInitialized, "foo"),
                Status(ErrorCodes::NetworkTimeout, "foo"),
                Status(ErrorCodes::MaxTimeMSExpired, "foo"),
                Status(ErrorCodes::CursorNotFound, "foo")};
    }

private:
    std::shared_ptr<ThreadPool> _executor;
};

TEST_F(WithAutomaticRetryTest, WithAutomaticRetryRetriesOnPredicateTrue) {
    auto errorToRetryOn = Status(ErrorCodes::IllegalOpMsgFlag, "foo");
    int numAttempts = 0;
    ExecutorFuture<void> future =
        WithAutomaticRetry(
            [this, &numAttempts, errorToRetryOn] {
                return ExecutorFuture<void>(getExecutor()).then([&numAttempts, errorToRetryOn] {
                    ++numAttempts;
                    if (numAttempts < 3) {
                        return errorToRetryOn;
                    }
                    return Status::OK();
                });
            },
            [errorToRetryOn](const Status& status) { return status == errorToRetryOn; })
            .onTransientError([](const Status& retryStatus) {})
            .onUnrecoverableError([](const Status& retryStatus) {})
            .until<Status>([](const Status& status) { return status.isOK(); })
            .on(getExecutor(), CancellationToken::uncancelable());

    ASSERT_OK(future.getNoThrow());
    ASSERT_EQ(numAttempts, 3);
}

TEST_F(WithAutomaticRetryTest, WithAutomaticRetryDoesNotRetryOnPredicateFalse) {
    auto errorToRetryOn = Status(ErrorCodes::IllegalOpMsgFlag, "foo");
    auto errorNotToRetryOn = Status(ErrorCodes::NotWritablePrimary, "foo");
    int numAttempts = 0;
    ExecutorFuture<void> future =
        WithAutomaticRetry(
            [this, &numAttempts, errorNotToRetryOn] {
                return ExecutorFuture<void>(getExecutor()).then([&numAttempts, errorNotToRetryOn] {
                    ++numAttempts;
                    if (numAttempts < 3) {
                        return errorNotToRetryOn;
                    }
                    return Status::OK();
                });
            },
            [errorToRetryOn](const Status& status) { return status == errorToRetryOn; })
            .onTransientError([](const Status& retryStatus) {})
            .onUnrecoverableError([](const Status& retryStatus) {})
            .until<Status>([](const Status& status) { return status.isOK(); })
            .on(getExecutor(), CancellationToken::uncancelable());

    ASSERT_NOT_OK(future.getNoThrow());
    ASSERT_EQ(numAttempts, 1);
}

TEST_F(WithAutomaticRetryTest, DefaultRetryabilityPredicateTest) {
    std::vector<Status> errorsToTest = getDefaultRetryableErrors();

    for (const auto& error : errorsToTest) {
        ASSERT_TRUE(kDefaultRetryabilityPredicate(error));
    }

    ASSERT_FALSE(kDefaultRetryabilityPredicate(Status(ErrorCodes::WriteConcernTimeout, "foo")));
}

TEST_F(WithAutomaticRetryTest, DefaultRetryWithWriteConcernError) {
    std::vector<Status> errorsToTest = getDefaultRetryableErrors();

    errorsToTest.emplace_back(Status(ErrorCodes::WriteConcernTimeout, "foo"));

    for (const auto& error : errorsToTest) {
        ASSERT_TRUE(kRetryabilityPredicateIncludeWriteConcernTimeout(error));
    }
}

}  // namespace primary_only_service_helpers
}  // namespace mongo

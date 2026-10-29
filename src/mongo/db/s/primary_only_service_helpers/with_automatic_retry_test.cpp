// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/primary_only_service_helpers/with_automatic_retry.h"

#include "mongo/base/status.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"

#include <memory>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace primary_only_service_helpers {

class WithAutomaticRetryTest : public unittest::Test {
protected:
    void setUp() override {
        _executor = executor::ThreadPoolTaskExecutor::create(
            ThreadPool::make({
                .maxThreads = 2,
            }),
            std::make_unique<executor::NetworkInterfaceMock>());
        _executor->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> getExecutor() const {
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
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
};

TEST_F(WithAutomaticRetryTest, WithAutomaticRetryRetriesOnPredicateTrue) {
    FailPointEnableBlock fp{"setBackoffDelayForTesting", BSON("backoffDelayMs" << 0)};
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
            .runOn(getExecutor(), CancellationToken::uncancelable());

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
            .runOn(getExecutor(), CancellationToken::uncancelable());

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

TEST_F(WithAutomaticRetryTest, DefaultRetryabilityPredicateDoesNotRetryOnReplicaSetWritesBlocked) {
    // ReplicaSetWritesBlocked is only retryable for specific operations that opt in via a custom
    // predicate; the default predicates must not treat it as retryable.
    auto error = Status(ErrorCodes::ReplicaSetWritesBlocked, "foo");
    ASSERT_FALSE(kDefaultRetryabilityPredicate(error));
    ASSERT_FALSE(kRetryabilityPredicateIncludeWriteConcernTimeout(error));
}

}  // namespace primary_only_service_helpers
}  // namespace mongo

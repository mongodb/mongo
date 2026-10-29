// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/exhaust_response_reader_tl.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo::executor {
namespace {

class ExhaustResponseReaderTLTest : public unittest::Test {
public:
    using Canceller = ExhaustResponseReaderTL::InFlightReadCanceller;

    Canceller state;
};

TEST_F(ExhaustResponseReaderTLTest, CancellationBeforeReadIsRemembered) {
    state.cancel();
    auto read = state.startRead();
    ASSERT_TRUE(read.token.isCanceled());
}

TEST_F(ExhaustResponseReaderTLTest, CancellationReachesActiveRead) {
    auto read = state.startRead();
    ASSERT_FALSE(read.token.isCanceled());

    state.cancel();
    ASSERT_TRUE(read.token.isCanceled());
}

TEST_F(ExhaustResponseReaderTLTest, CompletedReadIsNotCanceled) {
    auto read = state.startRead();
    state.finishRead(read.id);

    state.cancel();

    ASSERT_FALSE(read.token.isCanceled());
    ASSERT_FALSE(read.token.isCancelable());
}

TEST_F(ExhaustResponseReaderTLTest, CancellationBetweenReadsReachesOnlyNextRead) {
    auto first = state.startRead();
    state.finishRead(first.id);

    state.cancel();

    auto second = state.startRead();
    ASSERT_FALSE(first.token.isCanceled());
    ASSERT_TRUE(second.token.isCanceled());
}

TEST_F(ExhaustResponseReaderTLTest, LocalCancellationDoesNotCancelLaterReads) {
    auto first = state.startRead();

    state.cancelRead(first.id);

    ASSERT_TRUE(first.token.isCanceled());
    auto second = state.startRead();
    ASSERT_FALSE(second.token.isCanceled());
    ASSERT_TRUE(second.token.isCancelable());
}

TEST_F(ExhaustResponseReaderTLTest, LateLocalCancellationDoesNotCancelNextRead) {
    auto first = state.startRead();
    state.finishRead(first.id);
    auto second = state.startRead();
    ASSERT_NE(first.id, second.id);

    state.cancelRead(first.id);

    ASSERT_FALSE(first.token.isCanceled());
    ASSERT_FALSE(second.token.isCanceled());
    ASSERT_TRUE(second.token.isCancelable());
    state.cancel();
    ASSERT_TRUE(second.token.isCanceled());
}

TEST_F(ExhaustResponseReaderTLTest, LateReadCompletionDoesNotReleaseNextRead) {
    auto first = state.startRead();
    state.cancelRead(first.id);
    auto second = state.startRead();
    state.finishRead(first.id);

    ASSERT_TRUE(second.token.isCancelable());
    state.cancel();
    ASSERT_TRUE(second.token.isCanceled());
}

TEST_F(ExhaustResponseReaderTLTest, CancellationAfterFinishDoesNotReachActiveRead) {
    auto read = state.startRead();
    state.finish();
    state.cancel();

    ASSERT_FALSE(read.token.isCanceled());
}

TEST_F(ExhaustResponseReaderTLTest, ReadAfterFinishIsAlreadyCanceled) {
    auto first = state.startRead();
    state.finish();
    auto second = state.startRead();

    ASSERT_TRUE(second.token.isCanceled());
}

}  // namespace
}  // namespace mongo::executor

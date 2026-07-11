// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/cancellation.h"

#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(CancelTest, CancelSourceDestructorDoesNotCauseCancellation) {
    bool ran{false};
    {
        CancellationSource source;
        source.token().onCancel().unsafeToInlineFuture().getAsync([&ran](Status s) mutable {
            ASSERT_EQ(s, detail::getCancelNeverCalledOnSourceError());
            ran = true;
        });
    }

    ASSERT_TRUE(ran);
}

TEST(CancelTest, CancelTokenIsCanceledIsFalseBeforeCancellation) {
    CancellationSource source;
    ASSERT_FALSE(source.token().isCanceled());
}

TEST(CancelTest, CancelTokenOnCancelFutureIsNotReadyBeforeCancellation) {
    CancellationSource source;
    ASSERT_FALSE(source.token().onCancel().isReady());
}

TEST(CancelTest, CancelingSourceMakesExistingCancelTokenIsCanceledTrue) {
    CancellationSource source;
    auto token = source.token();
    source.cancel();
    ASSERT_TRUE(token.isCanceled());
}

TEST(CancelTest, CancelingSourceMakesCancelTokenIsCanceledTrueForTokensCreatedAfterCancellation) {
    CancellationSource source;
    source.cancel();
    ASSERT_TRUE(source.token().isCanceled());
}

TEST(CancelTest, CallingCancelOnCancellationSourceTwiceIsSafe) {
    CancellationSource source;
    source.cancel();
    source.cancel();
    ASSERT_TRUE(source.token().isCanceled());
}

TEST(CancelTest,
     CallingCancelOnACopyOfACancellationSourceTriggersCancellationInTokensObtainedFromOriginal) {
    CancellationSource source;
    auto token = source.token();
    auto copy = source;
    copy.cancel();
    ASSERT_TRUE(token.isCanceled());
}

TEST(CancelTest,
     DestroyingACopyOfACancellationSourceDoesNotSetErrorOnCancellationFutureFromOriginalSource) {
    CancellationSource source;
    auto token = source.token();
    {
        auto copy = source;
    }
    ASSERT_FALSE(token.onCancel().isReady());
}

TEST(CancelTest,
     DestroyingACancellationSourceWithAnExistingCopyDoesNotSetErrorOnCancellationFuture) {
    boost::optional<CancellationSource> source;
    source.emplace();
    auto copy = *source;
    source.reset();
    ASSERT_FALSE(copy.token().onCancel().isReady());
}

TEST(CancelTest, CancelingSourceTriggersOnCancelCallbacksOnSingleCancelToken) {
    CancellationSource source;
    auto token = source.token();
    bool cancelCallbackRan{false};
    auto result = token.onCancel().unsafeToInlineFuture().then([&] { cancelCallbackRan = true; });
    source.cancel();
    result.wait();
    ASSERT_TRUE(cancelCallbackRan);
}

TEST(CancelTest, CancelingSourceTriggersOnCancelCallbacksOnMultipleCancelTokens) {
    CancellationSource source;
    auto token1 = source.token();
    auto token2 = source.token();
    bool cancelCallback1Ran{false};
    bool cancelCallback2Ran{false};
    auto result1 =
        token1.onCancel().unsafeToInlineFuture().then([&] { cancelCallback1Ran = true; });
    auto result2 =
        token2.onCancel().unsafeToInlineFuture().then([&] { cancelCallback2Ran = true; });
    source.cancel();
    result1.wait();
    result2.wait();
    ASSERT_TRUE(cancelCallback1Ran);
    ASSERT_TRUE(cancelCallback2Ran);
}

TEST(CancelTest, CancelTokenOnCancelFutureIsReadyAndRunsCallbacksOnTokensCreatedAfterCancellation) {
    CancellationSource source;
    source.cancel();
    bool cancelCallbackRan{false};
    auto token = source.token();
    ASSERT_TRUE(token.onCancel().isReady());
    token.onCancel().unsafeToInlineFuture().then([&] { cancelCallbackRan = true; }).wait();
    ASSERT_TRUE(cancelCallbackRan);
}

TEST(CancelTest, CancelingSourceConstructedFromATokenDoesNotCancelThatToken) {
    CancellationSource parent;
    auto parentToken = parent.token();

    CancellationSource child(parentToken);
    child.cancel();

    ASSERT_FALSE(parentToken.isCanceled());
    ASSERT_TRUE(child.token().isCanceled());
}

TEST(CancelTest, CancelingTokenUsedToConstructASourceCallsCancelOnThatSource) {
    CancellationSource parent;
    CancellationSource child(parent.token());
    parent.cancel();

    ASSERT_TRUE(parent.token().isCanceled());
    ASSERT_TRUE(child.token().isCanceled());
}

TEST(CancelTest, CancelTokenRemembersIsCanceledForCanceledSourceEvenAfterSourceIsDestroyed) {
    auto token = [] {
        CancellationSource source;
        auto token = source.token();
        source.cancel();
        return token;
    }();
    ASSERT_TRUE(token.isCanceled());
    ASSERT_TRUE(token.onCancel().isReady());
    ASSERT_EQ(token.onCancel().getNoThrow(), Status::OK());
}

TEST(CancelTest, CancelTokenRemembersNotCanceledForNotCanceledSourceEvenAfterSourceIsDestroyed) {
    auto token = [] {
        CancellationSource source;
        auto token = source.token();
        return token;
    }();
    ASSERT_FALSE(token.isCanceled());
    ASSERT_TRUE(token.onCancel().isReady());
    ASSERT_EQ(token.onCancel().getNoThrow(), detail::getCancelNeverCalledOnSourceError());
}

TEST(CancelTest, UncancelableTokenReturnsFalseForIsCanceled) {
    auto token = CancellationToken::uncancelable();
    ASSERT_FALSE(token.isCanceled());
}

TEST(CancelTest, UncancelableTokenNeverRunsCallbacks) {
    auto token = CancellationToken::uncancelable();
    auto cancelFuture = token.onCancel();
    ASSERT_TRUE(cancelFuture.isReady());
    ASSERT_EQ(cancelFuture.getNoThrow(), detail::getCancelNeverCalledOnSourceError());
}

TEST(CancelTest, UncancelableTokenReturnsFalseForIsCancelable) {
    auto token = CancellationToken::uncancelable();
    ASSERT_FALSE(token.isCancelable());
}

TEST(CancelTest, TokenIsCancelableReturnsTrueIfSourceIsAlreadyCanceled) {
    CancellationSource source;
    auto token = source.token();
    ASSERT_TRUE(token.isCancelable());
}

TEST(CancelTest, TokenIsCancelableReturnsFalseIfSourceHasBeenDestroyedWithoutCancelBeingCalled) {
    auto token = [] {
        CancellationSource source;
        auto token = source.token();
        return token;
    }();
    ASSERT_FALSE(token.isCancelable());
}

TEST(CancelTest, TokenIsCancelableReturnsTrueIfSourceExistsAndIsNotYetCanceled) {
    CancellationSource source;
    auto token = source.token();
    ASSERT_TRUE(token.isCancelable());
}

}  // namespace
}  // namespace mongo

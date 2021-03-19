/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"

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
    { auto copy = source; }
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

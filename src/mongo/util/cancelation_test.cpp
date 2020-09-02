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
#include "mongo/util/cancelation.h"

namespace mongo {
namespace {

TEST(CancelTest, CancelSourceDestructorDoesNotCauseCancelation) {
    bool ran{false};
    {
        CancelationSource source;
        source.token().onCancel().unsafeToInlineFuture().getAsync([&ran](Status s) mutable {
            ASSERT_EQ(s, detail::kCancelNeverCalledOnSourceError);
            ran = true;
        });
    }

    ASSERT_TRUE(ran);
}

TEST(CancelTest, CancelTokenIsCanceledIsFalseBeforeCancelation) {
    CancelationSource source;
    ASSERT_FALSE(source.token().isCanceled());
}

TEST(CancelTest, CancelTokenOnCancelFutureIsNotReadyBeforeCancelation) {
    CancelationSource source;
    ASSERT_FALSE(source.token().onCancel().isReady());
}

TEST(CancelTest, CancelingSourceMakesExistingCancelTokenIsCanceledTrue) {
    CancelationSource source;
    auto token = source.token();
    source.cancel();
    ASSERT_TRUE(token.isCanceled());
}

TEST(CancelTest, CancelingSourceMakesCancelTokenIsCanceledTrueForTokensCreatedAfterCancelation) {
    CancelationSource source;
    source.cancel();
    ASSERT_TRUE(source.token().isCanceled());
}

TEST(CancelTest, CallingCancelOnCancelationSourceTwiceIsSafe) {
    CancelationSource source;
    source.cancel();
    source.cancel();
    ASSERT_TRUE(source.token().isCanceled());
}

TEST(CancelTest,
     CallingCancelOnACopyOfACancelationSourceTriggersCancelationInTokensObtainedFromOriginal) {
    CancelationSource source;
    auto token = source.token();
    auto copy = source;
    copy.cancel();
    ASSERT_TRUE(token.isCanceled());
}

TEST(CancelTest,
     DestroyingACopyOfACancelationSourceDoesNotSetErrorOnCancelationFutureFromOriginalSource) {
    CancelationSource source;
    auto token = source.token();
    { auto copy = source; }
    ASSERT_FALSE(token.onCancel().isReady());
}

TEST(CancelTest, DestroyingACancelationSourceWithAnExistingCopyDoesNotSetErrorOnCancelationFuture) {
    boost::optional<CancelationSource> source;
    source.emplace();
    auto copy = *source;
    source.reset();
    ASSERT_FALSE(copy.token().onCancel().isReady());
}

TEST(CancelTest, CancelingSourceTriggersOnCancelCallbacksOnSingleCancelToken) {
    CancelationSource source;
    auto token = source.token();
    bool cancelCallbackRan{false};
    auto result = token.onCancel().unsafeToInlineFuture().then([&] { cancelCallbackRan = true; });
    source.cancel();
    result.wait();
    ASSERT_TRUE(cancelCallbackRan);
}

TEST(CancelTest, CancelingSourceTriggersOnCancelCallbacksOnMultipleCancelTokens) {
    CancelationSource source;
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

TEST(CancelTest, CancelTokenOnCancelFutureIsReadyAndRunsCallbacksOnTokensCreatedAfterCancelation) {
    CancelationSource source;
    source.cancel();
    bool cancelCallbackRan{false};
    auto token = source.token();
    ASSERT_TRUE(token.onCancel().isReady());
    token.onCancel().unsafeToInlineFuture().then([&] { cancelCallbackRan = true; }).wait();
    ASSERT_TRUE(cancelCallbackRan);
}

TEST(CancelTest, CancelingSourceConstructedFromATokenDoesNotCancelThatToken) {
    CancelationSource parent;
    auto parentToken = parent.token();

    CancelationSource child(parentToken);
    child.cancel();

    ASSERT_FALSE(parentToken.isCanceled());
    ASSERT_TRUE(child.token().isCanceled());
}

TEST(CancelTest, CancelingTokenUsedToConstructASourceCallsCancelOnThatSource) {
    CancelationSource parent;
    CancelationSource child(parent.token());
    parent.cancel();

    ASSERT_TRUE(parent.token().isCanceled());
    ASSERT_TRUE(child.token().isCanceled());
}

TEST(CancelTest, CancelTokenRemembersIsCanceledForCanceledSourceEvenAfterSourceIsDestroyed) {
    auto token = [] {
        CancelationSource source;
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
        CancelationSource source;
        auto token = source.token();
        return token;
    }();
    ASSERT_FALSE(token.isCanceled());
    ASSERT_TRUE(token.onCancel().isReady());
    ASSERT_EQ(token.onCancel().getNoThrow(), detail::kCancelNeverCalledOnSourceError);
}

TEST(CancelTest, UncancelableTokenReturnsFalseForIsCanceled) {
    auto token = CancelationToken::uncancelable();
    ASSERT_FALSE(token.isCanceled());
}

TEST(CancelTest, UncancelableTokenNeverRunsCallbacks) {
    auto token = CancelationToken::uncancelable();
    auto cancelFuture = token.onCancel();
    ASSERT_TRUE(cancelFuture.isReady());
    ASSERT_EQ(cancelFuture.getNoThrow(), detail::kCancelNeverCalledOnSourceError);
}

TEST(CancelTest, UncancelableTokenReturnsFalseForIsCancelable) {
    auto token = CancelationToken::uncancelable();
    ASSERT_FALSE(token.isCancelable());
}

TEST(CancelTest, TokenIsCancelableReturnsTrueIfSourceIsAlreadyCanceled) {
    CancelationSource source;
    auto token = source.token();
    ASSERT_TRUE(token.isCancelable());
}

TEST(CancelTest, TokenIsCancelableReturnsFalseIfSourceHasBeenDestroyedWithoutCancelBeingCalled) {
    auto token = [] {
        CancelationSource source;
        auto token = source.token();
        return token;
    }();
    ASSERT_FALSE(token.isCancelable());
}

TEST(CancelTest, TokenIsCancelableReturnsTrueIfSourceExistsAndIsNotYetCanceled) {
    CancelationSource source;
    auto token = source.token();
    ASSERT_TRUE(token.isCancelable());
}

}  // namespace
}  // namespace mongo

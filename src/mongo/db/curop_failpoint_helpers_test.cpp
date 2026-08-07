// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/curop_failpoint_helpers.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

class CurOpFailpointHelpersTest : public ServiceContextTest {
protected:
    ServiceContext::UniqueOperationContext makeOpCtx() {
        return getClient()->makeOperationContext();
    }
};

// Regression test for a matcher bug: a failpoint scoped to a specific 'comment' must never match
// an operation that has no comment at all, even if the failpoint's data also carries an 'nss' that
// happens to match the operation's namespace. The buggy version fell back to nss-only matching
// whenever the *candidate operation* (not the failpoint's data) lacked a comment, which let
// unrelated, comment-less operations (e.g. background consistency-check queries) get wrongly
// ensnared by a failpoint intended for one specific tagged query — and since 'FailPoint::setMode'
// spins forever waiting for such waiters to drain, this produced an unrecoverable hang.
TEST_F(CurOpFailpointHelpersTest, CommentScopedFailPointDoesNotMatchCommentlessOperation) {
    FailPoint failPoint("testCommentScopedFailPoint");
    failPoint.setMode(FailPoint::alwaysOn, 0, BSON("comment" << "targeted-comment"));

    auto opCtx = makeOpCtx();
    // Deliberately do not set a comment on this operation context.
    ASSERT_FALSE(opCtx->getComment());

    int timesEnteredWaitLoop = 0;
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &failPoint,
        opCtx.get(),
        "testFailpointMsg",
        [&] { timesEnteredWaitLoop++; },
        NamespaceString::createNamespaceString_forTest("test.coll"));

    // The predicate should have returned false for this comment-less operation, so the wait loop
    // must never have been entered — the call should return immediately.
    ASSERT_EQ(0, timesEnteredWaitLoop);
}

TEST_F(CurOpFailpointHelpersTest, CommentScopedFailPointMatchesOperationWithEqualComment) {
    FailPoint failPoint("testCommentScopedFailPoint");
    // 'sleepFor' makes the wait loop run exactly one iteration and then return, so this test
    // doesn't need a second thread to disable the failpoint.
    failPoint.setMode(
        FailPoint::alwaysOn, 0, BSON("comment" << "targeted-comment" << "sleepFor" << 1));

    auto opCtx = makeOpCtx();
    opCtx->setComment(BSON("comment" << "targeted-comment"));

    int timesEnteredWaitLoop = 0;
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &failPoint,
        opCtx.get(),
        "testFailpointMsg",
        [&] { timesEnteredWaitLoop++; },
        NamespaceString::createNamespaceString_forTest("test.coll"));

    ASSERT_EQ(1, timesEnteredWaitLoop);
}

// A 'comment' in the failpoint's data takes precedence over an 'nss': if the comment identifies the
// operation, the failpoint matches even when the operation runs on a different namespace than the
// one in the data.
TEST_F(CurOpFailpointHelpersTest, CommentScopedFailPointMatchesEvenWhenNamespaceDiffers) {
    FailPoint failPoint("testCommentScopedFailPoint");
    failPoint.setMode(FailPoint::alwaysOn,
                      0,
                      BSON("comment" << "targeted-comment" << "nss" << "test.otherColl"
                                     << "sleepFor" << 1));

    auto opCtx = makeOpCtx();
    opCtx->setComment(BSON("comment" << "targeted-comment"));

    int timesEnteredWaitLoop = 0;
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &failPoint,
        opCtx.get(),
        "testFailpointMsg",
        [&] { timesEnteredWaitLoop++; },
        NamespaceString::createNamespaceString_forTest("test.coll"));

    ASSERT_EQ(1, timesEnteredWaitLoop);
}

// A namespace-scoped failpoint would silently degrade to matching every operation reaching the
// callsite if the callsite forgot to pass the operation's namespace, so that combination must fail
// loudly instead.
TEST_F(CurOpFailpointHelpersTest, NssScopedFailPointUassertsWhenCallsitePassesNoNamespace) {
    FailPoint failPoint("testNssScopedFailPoint");
    failPoint.setMode(FailPoint::alwaysOn, 0, BSON("nss" << "test.coll" << "sleepFor" << 1));

    auto opCtx = makeOpCtx();

    int timesEnteredWaitLoop = 0;
    ASSERT_THROWS_CODE(
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &failPoint, opCtx.get(), "testFailpointMsg", [&] { timesEnteredWaitLoop++; }),
        DBException,
        13238000);
    ASSERT_EQ(0, timesEnteredWaitLoop);
}

// An empty 'nss' in the data means "do not scope by namespace", so it matches an operation at a
// callsite which passes no namespace instead of tripping the uassert above.
TEST_F(CurOpFailpointHelpersTest, EmptyNssInDataMatchesWhenCallsitePassesNoNamespace) {
    FailPoint failPoint("testNssScopedFailPoint");
    failPoint.setMode(FailPoint::alwaysOn, 0, BSON("nss" << "" << "sleepFor" << 1));

    auto opCtx = makeOpCtx();

    int timesEnteredWaitLoop = 0;
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &failPoint, opCtx.get(), "testFailpointMsg", [&] { timesEnteredWaitLoop++; });

    ASSERT_EQ(1, timesEnteredWaitLoop);
}

TEST_F(CurOpFailpointHelpersTest, NssScopedFailPointFallbackStillMatchesWhenNoCommentInData) {
    FailPoint failPoint("testNssScopedFailPoint");
    failPoint.setMode(FailPoint::alwaysOn, 0, BSON("nss" << "test.coll" << "sleepFor" << 1));

    auto opCtx = makeOpCtx();
    // No comment set; the failpoint's data has no 'comment' field either, so this should fall
    // back to (and satisfy) the nss check.
    ASSERT_FALSE(opCtx->getComment());

    int timesEnteredWaitLoop = 0;
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &failPoint,
        opCtx.get(),
        "testFailpointMsg",
        [&] { timesEnteredWaitLoop++; },
        NamespaceString::createNamespaceString_forTest("test.coll"));

    ASSERT_EQ(1, timesEnteredWaitLoop);
}

TEST_F(CurOpFailpointHelpersTest, NssScopedFailPointDoesNotMatchDifferentNamespace) {
    FailPoint failPoint("testNssScopedFailPoint");
    failPoint.setMode(FailPoint::alwaysOn, 0, BSON("nss" << "test.otherColl" << "sleepFor" << 1));

    auto opCtx = makeOpCtx();

    int timesEnteredWaitLoop = 0;
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &failPoint,
        opCtx.get(),
        "testFailpointMsg",
        [&] { timesEnteredWaitLoop++; },
        NamespaceString::createNamespaceString_forTest("test.coll"));

    ASSERT_EQ(0, timesEnteredWaitLoop);
}

}  // namespace
}  // namespace mongo

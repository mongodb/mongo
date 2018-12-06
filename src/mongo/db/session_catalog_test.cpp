
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

class SessionCatalogTest : public ServiceContextTest {
protected:
    SessionCatalog* catalog() {
        return SessionCatalog::get(getServiceContext());
    }
};

class SessionCatalogTestWithDefaultOpCtx : public SessionCatalogTest {
protected:
    const ServiceContext::UniqueOperationContext _uniqueOpCtx = makeOperationContext();
    OperationContext* const _opCtx = _uniqueOpCtx.get();
};

// When this class is in scope, makes the system behave as if we're in a DBDirectClient
class DirectClientSetter {
public:
    explicit DirectClientSetter(OperationContext* opCtx)
        : _opCtx(opCtx), _wasInDirectClient(_opCtx->getClient()->isInDirectClient()) {
        _opCtx->getClient()->setInDirectClient(true);
    }

    ~DirectClientSetter() {
        _opCtx->getClient()->setInDirectClient(_wasInDirectClient);
    }

private:
    const OperationContext* _opCtx;
    const bool _wasInDirectClient;
};

TEST_F(SessionCatalogTestWithDefaultOpCtx, CheckoutAndReleaseSession) {
    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());

    auto scopedSession = catalog()->checkOutSession(_opCtx);

    ASSERT(scopedSession.get());
    ASSERT_EQ(*_opCtx->getLogicalSessionId(), scopedSession->getSessionId());
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, OperationContextCheckedOutSession) {
    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    const TxnNumber txnNum = 20;
    _opCtx->setTxnNumber(txnNum);

    OperationContextSession ocs(_opCtx);
    auto session = OperationContextSession::get(_opCtx);
    ASSERT(session);
    ASSERT_EQ(*_opCtx->getLogicalSessionId(), session->getSessionId());
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, NestedOperationContextSession) {
    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        OperationContextSession outerScopedSession(_opCtx);

        {
            DirectClientSetter inDirectClient(_opCtx);
            OperationContextSession innerScopedSession(_opCtx);

            auto session = OperationContextSession::get(_opCtx);
            ASSERT(session);
            ASSERT_EQ(*_opCtx->getLogicalSessionId(), session->getSessionId());
        }

        {
            DirectClientSetter inDirectClient(_opCtx);
            auto session = OperationContextSession::get(_opCtx);
            ASSERT(session);
            ASSERT_EQ(*_opCtx->getLogicalSessionId(), session->getSessionId());
        }
    }

    ASSERT(!OperationContextSession::get(_opCtx));
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, ScanSessions) {
    std::vector<LogicalSessionId> lsidsFound;
    const auto workerFn = [&lsidsFound](WithLock, Session* session) {
        lsidsFound.push_back(session->getSessionId());
    };

    // Scan over zero Sessions.
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx)});
    catalog()->scanSessions(matcherAllSessions, workerFn);
    ASSERT(lsidsFound.empty());
    lsidsFound.clear();

    // Create three sessions in the catalog.
    const std::vector<LogicalSessionId> lsids{makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest()};
    for (const auto& lsid : lsids) {
        stdx::async(stdx::launch::async, [this, lsid] {
            ThreadClient tc(getServiceContext());
            auto opCtx = makeOperationContext();
            const auto unusedSession(catalog()->checkOutSession(opCtx.get(), lsid));
        }).get();
    }

    // Scan over all Sessions.
    catalog()->scanSessions(matcherAllSessions, workerFn);
    ASSERT_EQ(3U, lsidsFound.size());
    lsidsFound.clear();

    // Scan over all Sessions, visiting a particular Session.
    SessionKiller::Matcher matcherLSID2(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx, lsids[2])});
    catalog()->scanSessions(matcherLSID2, workerFn);
    ASSERT_EQ(1U, lsidsFound.size());
    ASSERT_EQ(lsids[2], lsidsFound.front());
    lsidsFound.clear();
}

TEST_F(SessionCatalogTest, KillSessionWhenSessionIsNotCheckedOut) {
    const auto lsid = makeLogicalSessionIdForTest();

    // Create the session so there is something to kill
    {
        auto opCtx = makeOperationContext();
        const auto unusedSession(catalog()->checkOutSession(opCtx.get(), lsid));
    }

    auto killToken = catalog()->killSession(lsid);

    // Make sure that regular session check-out will fail because the session is marked as killed
    {
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        opCtx->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);
        ASSERT_THROWS_CODE(
            OperationContextSession(opCtx.get()), AssertionException, ErrorCodes::MaxTimeMSExpired);
    }

    // Schedule a separate "regular operation" thread, which will block on checking-out the session,
    // which we will use to confirm that session kill completion actually unblocks check-out
    auto future = stdx::async(stdx::launch::async, [this, lsid] {
        ThreadClient tc(getServiceContext());
        auto sideOpCtx = Client::getCurrent()->makeOperationContext();
        const auto unusedSession(catalog()->checkOutSession(sideOpCtx.get(), lsid));
    });
    ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

    // Make sure that "for kill" session check-out succeeds
    {
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
        ASSERT_EQ(opCtx.get(), scopedSession->currentOperation());
    }

    // Make sure that session check-out after kill succeeds again
    {
        auto opCtx = makeOperationContext();
        const auto unusedSession(catalog()->checkOutSession(opCtx.get(), lsid));
    }

    // Make sure the "regular operation" eventually is able to proceed and use the just killed
    // session
    future.get();
}

TEST_F(SessionCatalogTest, KillSessionWhenSessionIsCheckedOut) {
    const auto lsid = makeLogicalSessionIdForTest();

    auto killToken = [this, &lsid] {
        // Create the session so there is something to kill
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        OperationContextSession operationContextSession(opCtx.get());

        auto killToken = catalog()->killSession(lsid);

        // Make sure the owning operation context is interrupted
        ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), AssertionException, ErrorCodes::Interrupted);

        // Make sure that the checkOutForKill call will wait for the owning operation context to
        // check the session back in
        auto future = stdx::async(stdx::launch::async, [this, lsid] {
            ThreadClient tc(getServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(lsid);
            sideOpCtx->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);

            const auto unusedSession(catalog()->checkOutSession(sideOpCtx.get(), lsid));
        });

        ASSERT_THROWS_CODE(future.get(), AssertionException, ErrorCodes::MaxTimeMSExpired);

        return killToken;
    }();

    // Make sure that regular session check-out will fail because the session is marked as killed
    {
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        opCtx->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);
        ASSERT_THROWS_CODE(
            OperationContextSession(opCtx.get()), AssertionException, ErrorCodes::MaxTimeMSExpired);
    }

    // Schedule a separate "regular operation" thread, which will block on checking-out the session,
    // which we will use to confirm that session kill completion actually unblocks check-out
    auto future = stdx::async(stdx::launch::async, [this, lsid] {
        ThreadClient tc(getServiceContext());
        auto sideOpCtx = Client::getCurrent()->makeOperationContext();
        const auto unusedSession(catalog()->checkOutSession(sideOpCtx.get(), lsid));
    });
    ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

    // Make sure that "for kill" session check-out succeeds
    {
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
        ASSERT_EQ(opCtx.get(), scopedSession->currentOperation());
    }

    // Make sure that session check-out after kill succeeds again
    {
        auto opCtx = makeOperationContext();
        const auto unusedSession(catalog()->checkOutSession(opCtx.get(), lsid));
    }

    // Make sure the "regular operation" eventually is able to proceed and use the just killed
    // session
    future.get();
}

TEST_F(SessionCatalogTest, MarkSessionAsKilledCanBeCalledMoreThanOnce) {
    const auto lsid = makeLogicalSessionIdForTest();

    // Create the session so there is something to kill
    {
        auto opCtx = makeOperationContext();
        const auto unusedSession(catalog()->checkOutSession(opCtx.get(), lsid));
    }

    auto killToken1 = catalog()->killSession(lsid);
    auto killToken2 = catalog()->killSession(lsid);

    // Make sure that regular session check-out will fail because there are two killers on the
    // session
    {
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        opCtx->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);
        ASSERT_THROWS_CODE(
            OperationContextSession(opCtx.get()), AssertionException, ErrorCodes::MaxTimeMSExpired);
    }

    boost::optional<Session::KillToken> killTokenWhileSessionIsCheckedOutForKill;

    // Finish the first killer of the session
    {
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken1));
        ASSERT_EQ(opCtx.get(), scopedSession->currentOperation());

        // Killing a session while checked out for kill should not affect the killers
        killTokenWhileSessionIsCheckedOutForKill.emplace(catalog()->killSession(lsid));
    }

    // Regular session check-out should still fail because there are now still two killers on the
    // session
    {
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        opCtx->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);
        ASSERT_THROWS_CODE(
            OperationContextSession(opCtx.get()), AssertionException, ErrorCodes::MaxTimeMSExpired);
    }
    {
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken2));
        ASSERT_EQ(opCtx.get(), scopedSession->currentOperation());
    }
    {
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(
            opCtx.get(), std::move(*killTokenWhileSessionIsCheckedOutForKill));
        ASSERT_EQ(opCtx.get(), scopedSession->currentOperation());
    }
}

TEST_F(SessionCatalogTest, MarkSessionsAsKilledWhenSessionDoesNotExist) {
    const auto nonExistentLsid = makeLogicalSessionIdForTest();
    ASSERT_THROWS_CODE(
        catalog()->killSession(nonExistentLsid), AssertionException, ErrorCodes::NoSuchSession);
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, SessionDiscarOperationContextAfterCheckIn) {
    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        OperationContextSession ocs(_opCtx);
        ASSERT(OperationContextSession::get(_opCtx));

        OperationContextSession::checkIn(_opCtx);
        ASSERT(!OperationContextSession::get(_opCtx));
    }

    ASSERT(!OperationContextSession::get(_opCtx));
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, SessionDiscarOperationContextAfterCheckInCheckOut) {
    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        OperationContextSession ocs(_opCtx);
        ASSERT(OperationContextSession::get(_opCtx));

        OperationContextSession::checkIn(_opCtx);
        ASSERT(!OperationContextSession::get(_opCtx));

        OperationContextSession::checkOut(_opCtx);
        ASSERT(OperationContextSession::get(_opCtx));
    }

    ASSERT(!OperationContextSession::get(_opCtx));
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, KillSessionsThroughScanSessions) {
    // Create three sessions
    const std::vector<LogicalSessionId> lsids{makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest()};

    std::vector<stdx::future<void>> futures;
    unittest::Barrier firstUseOfTheSessionReachedBarrier(lsids.size() + 1);

    for (const auto& lsid : lsids) {
        futures.emplace_back(
            stdx::async(stdx::launch::async, [this, lsid, &firstUseOfTheSessionReachedBarrier] {
                ThreadClient tc(getServiceContext());

                {
                    auto sideOpCtx = Client::getCurrent()->makeOperationContext();
                    const auto unusedSession(catalog()->checkOutSession(sideOpCtx.get(), lsid));

                    firstUseOfTheSessionReachedBarrier.countDownAndWait();

                    ASSERT_THROWS_CODE(sideOpCtx->sleepFor(Hours{6}),
                                       AssertionException,
                                       ErrorCodes::ExceededTimeLimit);
                }

                {
                    auto sideOpCtx = Client::getCurrent()->makeOperationContext();
                    sideOpCtx->setLogicalSessionId(lsid);

                    const auto unusedSession(catalog()->checkOutSession(sideOpCtx.get(), lsid));
                }
            }));
    }

    // Make sure all spawned threads have created the session
    firstUseOfTheSessionReachedBarrier.countDownAndWait();

    // Kill the first and the third sessions
    {
        std::vector<Session::KillToken> firstAndThirdTokens;
        catalog()->scanSessions(
            SessionKiller::Matcher(
                KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx)}),
            [&lsids, &firstAndThirdTokens](WithLock sessionCatalogLock, Session* session) {
                if (session->getSessionId() == lsids[0] || session->getSessionId() == lsids[2])
                    firstAndThirdTokens.emplace_back(
                        session->kill(sessionCatalogLock, ErrorCodes::ExceededTimeLimit));
            });
        ASSERT_EQ(2U, firstAndThirdTokens.size());
        for (auto& killToken : firstAndThirdTokens) {
            auto unusedSheckedOutSessionForKill(
                catalog()->checkOutSessionForKill(_opCtx, std::move(killToken)));
        }
        futures[0].get();
        futures[2].get();
    }

    // Kill the second session
    {
        std::vector<Session::KillToken> secondToken;
        catalog()->scanSessions(
            SessionKiller::Matcher(
                KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx)}),
            [&lsids, &secondToken](WithLock sessionCatalogLock, Session* session) {
                if (session->getSessionId() == lsids[1])
                    secondToken.emplace_back(
                        session->kill(sessionCatalogLock, ErrorCodes::ExceededTimeLimit));
            });
        ASSERT_EQ(1U, secondToken.size());
        for (auto& killToken : secondToken) {
            auto unusedSheckedOutSessionForKill(
                catalog()->checkOutSessionForKill(_opCtx, std::move(killToken)));
        }
        futures[1].get();
    }
}

}  // namespace
}  // namespace mongo

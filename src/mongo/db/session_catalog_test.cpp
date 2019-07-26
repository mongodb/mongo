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

#include <memory>

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/future.h"
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
    OperationContextSession ocs(_opCtx);

    auto session = OperationContextSession::get(_opCtx);
    ASSERT(session);
    ASSERT_EQ(*_opCtx->getLogicalSessionId(), session->getSessionId());
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

TEST_F(SessionCatalogTest, ScanSession) {
    // Create three sessions in the catalog.
    const std::vector<LogicalSessionId> lsids{makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest()};
    for (const auto& lsid : lsids) {
        stdx::async(stdx::launch::async,
                    [this, lsid] {
                        ThreadClient tc(getServiceContext());
                        auto opCtx = makeOperationContext();
                        opCtx->setLogicalSessionId(lsid);
                        OperationContextSession ocs(opCtx.get());
                    })
            .get();
    }

    catalog()->scanSession(lsids[0], [&lsids](const ObservableSession& session) {
        ASSERT_EQ(lsids[0], session.get()->getSessionId());
    });

    catalog()->scanSession(lsids[1], [&lsids](const ObservableSession& session) {
        ASSERT_EQ(lsids[1], session.get()->getSessionId());
    });

    catalog()->scanSession(lsids[2], [&lsids](const ObservableSession& session) {
        ASSERT_EQ(lsids[2], session.get()->getSessionId());
    });

    catalog()->scanSession(makeLogicalSessionIdForTest(), [](const ObservableSession&) {
        FAIL("The callback was called for non-existent session");
    });
}

TEST_F(SessionCatalogTest, ScanSessionMarkForReapWhenSessionIsIdle) {
    // Create three sessions in the catalog.
    const std::vector<LogicalSessionId> lsids{makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest()};
    for (const auto& lsid : lsids) {
        stdx::async(stdx::launch::async,
                    [this, lsid] {
                        ThreadClient tc(getServiceContext());
                        auto opCtx = makeOperationContext();
                        opCtx->setLogicalSessionId(lsid);
                        OperationContextSession ocs(opCtx.get());
                    })
            .get();
    }

    catalog()->scanSession(lsids[0],
                           [&lsids](ObservableSession& session) { session.markForReap(); });

    catalog()->scanSession(lsids[0], [](const ObservableSession&) {
        FAIL("The callback was called for non-existent session");
    });

    catalog()->scanSession(lsids[1], [&lsids](const ObservableSession& session) {
        ASSERT_EQ(lsids[1], session.get()->getSessionId());
    });

    catalog()->scanSession(lsids[2], [&lsids](const ObservableSession& session) {
        ASSERT_EQ(lsids[2], session.get()->getSessionId());
    });
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, ScanSessions) {
    std::vector<LogicalSessionId> lsidsFound;
    const auto workerFn = [&lsidsFound](const ObservableSession& session) {
        lsidsFound.push_back(session.getSessionId());
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
        stdx::async(stdx::launch::async,
                    [this, lsid] {
                        ThreadClient tc(getServiceContext());
                        auto opCtx = makeOperationContext();
                        opCtx->setLogicalSessionId(lsid);
                        OperationContextSession ocs(opCtx.get());
                    })
            .get();
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

TEST_F(SessionCatalogTestWithDefaultOpCtx, ScanSessionsMarkForReap) {
    // Create three sessions in the catalog.
    const std::vector<LogicalSessionId> lsids{makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest(),
                                              makeLogicalSessionIdForTest()};

    unittest::Barrier sessionsCheckedOut(2);
    unittest::Barrier sessionsCheckedIn(2);

    auto f = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsids[1]);
        OperationContextSession ocs(opCtx.get());
        sessionsCheckedOut.countDownAndWait();
        sessionsCheckedIn.countDownAndWait();
    });

    // After this wait, session 1 is checked-out and waiting on the barrier, because of which only
    // sessions 0 and 2 will be reaped
    sessionsCheckedOut.countDownAndWait();

    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx)});

    catalog()->scanSessions(matcherAllSessions,
                            [&](ObservableSession& session) { session.markForReap(); });

    catalog()->scanSessions(matcherAllSessions, [&](const ObservableSession& session) {
        ASSERT_EQ(lsids[1], session.get()->getSessionId());
    });

    // After this point, session 1 is checked back in
    sessionsCheckedIn.countDownAndWait();
    f.get();

    catalog()->scanSessions(matcherAllSessions, [&](const ObservableSession& session) {
        ASSERT_EQ(lsids[1], session.get()->getSessionId());
    });
}

TEST_F(SessionCatalogTest, KillSessionWhenSessionIsNotCheckedOut) {
    const auto lsid = makeLogicalSessionIdForTest();

    // Create the session so there is something to kill
    {
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        OperationContextSession ocs(opCtx.get());
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
        sideOpCtx->setLogicalSessionId(lsid);
        OperationContextSession ocs(sideOpCtx.get());
    });
    ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

    // Make sure that "for kill" session check-out succeeds
    {
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
        ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
    }

    // Make sure that session check-out after kill succeeds again
    {
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        OperationContextSession ocs(opCtx.get());
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
            OperationContextSession ocs(sideOpCtx.get());
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
        sideOpCtx->setLogicalSessionId(lsid);
        OperationContextSession ocs(sideOpCtx.get());
    });
    ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

    // Make sure that "for kill" session check-out succeeds
    {
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
        ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
    }

    // Make sure that session check-out after kill succeeds again
    {
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        OperationContextSession ocs(opCtx.get());
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
        opCtx->setLogicalSessionId(lsid);
        OperationContextSession ocs(opCtx.get());
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

    boost::optional<SessionCatalog::KillToken> killTokenWhileSessionIsCheckedOutForKill;

    // Finish the first killer of the session
    {
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken1));
        ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());

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
        ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
    }
    {
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(
            opCtx.get(), std::move(*killTokenWhileSessionIsCheckedOutForKill));
        ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
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
                    sideOpCtx->setLogicalSessionId(lsid);
                    OperationContextSession ocs(sideOpCtx.get());

                    firstUseOfTheSessionReachedBarrier.countDownAndWait();

                    ASSERT_THROWS_CODE(sideOpCtx->sleepFor(Hours{6}),
                                       AssertionException,
                                       ErrorCodes::ExceededTimeLimit);
                }

                {
                    auto sideOpCtx = Client::getCurrent()->makeOperationContext();
                    sideOpCtx->setLogicalSessionId(lsid);
                    OperationContextSession ocs(sideOpCtx.get());
                }
            }));
    }

    // Make sure all spawned threads have created the session
    firstUseOfTheSessionReachedBarrier.countDownAndWait();

    // Kill the first and the third sessions
    {
        std::vector<SessionCatalog::KillToken> firstAndThirdTokens;
        catalog()->scanSessions(
            SessionKiller::Matcher(
                KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx)}),
            [&lsids, &firstAndThirdTokens](const ObservableSession& session) {
                if (session.getSessionId() == lsids[0] || session.getSessionId() == lsids[2])
                    firstAndThirdTokens.emplace_back(session.kill(ErrorCodes::ExceededTimeLimit));
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
        std::vector<SessionCatalog::KillToken> secondToken;
        catalog()->scanSessions(
            SessionKiller::Matcher(
                KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx)}),
            [&lsids, &secondToken](const ObservableSession& session) {
                if (session.getSessionId() == lsids[1])
                    secondToken.emplace_back(session.kill(ErrorCodes::ExceededTimeLimit));
            });
        ASSERT_EQ(1U, secondToken.size());
        for (auto& killToken : secondToken) {
            auto unusedSheckedOutSessionForKill(
                catalog()->checkOutSessionForKill(_opCtx, std::move(killToken)));
        }
        futures[1].get();
    }
}

// Test that session kill will block normal sesion chechout and will be signaled correctly.
// Even if the implementaion has a bug, the test may not always fail depending on thread
// scheduling, however, this test case still gives us a good coverage.
TEST_F(SessionCatalogTestWithDefaultOpCtx, ConcurrentCheckOutAndKill) {
    auto lsid = makeLogicalSessionIdForTest();
    _opCtx->setLogicalSessionId(lsid);

    stdx::future<void> normalCheckOutFinish, killCheckOutFinish;

    // This variable is protected by the session check-out.
    std::string lastSessionCheckOut = "first session";
    {
        // Check out the session to block both normal check-out and checkOutForKill.
        OperationContextSession firstCheckOut(_opCtx);

        // Normal check out should start after kill.
        normalCheckOutFinish = stdx::async(stdx::launch::async, [&] {
            ThreadClient tc(getGlobalServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(lsid);
            OperationContextSession normalCheckOut(sideOpCtx.get());
            ASSERT_EQ("session kill", lastSessionCheckOut);
            lastSessionCheckOut = "session checkout";
        });

        // Kill will short-cut the queue and be the next one to check out.
        killCheckOutFinish = stdx::async(stdx::launch::async, [&] {
            ThreadClient tc(getGlobalServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(lsid);

            // Kill the session
            std::vector<SessionCatalog::KillToken> killTokens;
            catalog()->scanSessions(
                SessionKiller::Matcher(
                    KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(sideOpCtx.get())}),
                [&killTokens](const ObservableSession& session) {
                    killTokens.emplace_back(session.kill(ErrorCodes::InternalError));
                });
            ASSERT_EQ(1U, killTokens.size());
            auto checkOutSessionForKill(
                catalog()->checkOutSessionForKill(sideOpCtx.get(), std::move(killTokens[0])));
            ASSERT_EQ("first session", lastSessionCheckOut);
            lastSessionCheckOut = "session kill";
        });

        // The main thread won't check in the session until it's killed.
        {
            stdx::mutex m;
            stdx::condition_variable cond;
            stdx::unique_lock<stdx::mutex> lock(m);
            ASSERT_EQ(ErrorCodes::InternalError,
                      _opCtx->waitForConditionOrInterruptNoAssert(cond, lock));
        }
    }
    normalCheckOutFinish.get();
    killCheckOutFinish.get();
    ASSERT_EQ("session checkout", lastSessionCheckOut);
}

}  // namespace
}  // namespace mongo

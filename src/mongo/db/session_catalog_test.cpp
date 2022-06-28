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

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session_catalog.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

class SessionCatalogTest : public ServiceContextTest {
protected:
    SessionCatalog* catalog() {
        return SessionCatalog::get(getServiceContext());
    }

    void assertCanCheckoutSession(const LogicalSessionId& lsid) {
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        OperationContextSession ocs(opCtx.get());
    }

    void assertSessionCheckoutTimesOut(const LogicalSessionId& lsid) {
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        opCtx->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);
        ASSERT_THROWS_CODE(
            OperationContextSession(opCtx.get()), AssertionException, ErrorCodes::MaxTimeMSExpired);
    }

    void assertConcurrentCheckoutTimesOut(const LogicalSessionId& lsid) {
        auto future = stdx::async(stdx::launch::async, [this, lsid] {
            ThreadClient tc(getServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(lsid);
            sideOpCtx->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);
            OperationContextSession ocs(sideOpCtx.get());
        });
        ASSERT_THROWS_CODE(future.get(), AssertionException, ErrorCodes::MaxTimeMSExpired);
    }

    /**
     * Creates the session with the given 'lsid' by checking it out from the SessionCatalog and then
     * checking it back in.
     */
    void createSession(const LogicalSessionId& lsid) {
        stdx::async(stdx::launch::async,
                    [this, lsid] {
                        ThreadClient tc(getServiceContext());
                        auto opCtx = makeOperationContext();
                        opCtx->setLogicalSessionId(lsid);
                        OperationContextSession ocs(opCtx.get());
                    })
            .get();
    }

    /**
     * Returns the session ids for all sessions in the SessionCatalog.
     */
    std::vector<LogicalSessionId> getAllSessionIds(OperationContext* opCtx) {
        std::vector<LogicalSessionId> lsidsFound;
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        const auto getAllSessionIdsWorkerFn = [&lsidsFound](const ObservableSession& session) {
            lsidsFound.push_back(session.getSessionId());
        };
        catalog()->scanSessions(matcherAllSessions, getAllSessionIdsWorkerFn);
        return lsidsFound;
    };
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

TEST_F(SessionCatalogTest, GetParentSessionId) {
    auto parentLsid = makeLogicalSessionIdForTest();
    ASSERT(!getParentSessionId(parentLsid).has_value());
    ASSERT_EQ(parentLsid,
              *getParentSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid)));
    ASSERT_EQ(parentLsid, *getParentSessionId(makeLogicalSessionIdWithTxnUUIDForTest(parentLsid)));
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, CheckoutAndReleaseSession) {
    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    OperationContextSession ocs(_opCtx);

    auto session = OperationContextSession::get(_opCtx);
    ASSERT(session);
    ASSERT_EQ(*_opCtx->getLogicalSessionId(), session->getSessionId());
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, CheckoutAndReleaseSessionWithTxnNumber) {
    auto parentLsid = makeLogicalSessionIdForTest();
    auto childLsid = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid);
    _opCtx->setLogicalSessionId(childLsid);
    OperationContextSession ocs(_opCtx);

    auto session = OperationContextSession::get(_opCtx);
    auto parentSession = session->getParentSession();
    ASSERT(session);
    ASSERT_EQ(childLsid, session->getSessionId());
    ASSERT(parentSession);
    ASSERT_EQ(parentLsid, parentSession->getSessionId());
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, CheckoutAndReleaseSessionWithTxnUUID) {
    auto parentLsid = makeLogicalSessionIdForTest();
    auto childLsid = makeLogicalSessionIdWithTxnUUIDForTest(parentLsid);
    _opCtx->setLogicalSessionId(childLsid);
    OperationContextSession ocs(_opCtx);

    auto session = OperationContextSession::get(_opCtx);
    auto parentSession = session->getParentSession();
    ASSERT(session);
    ASSERT_EQ(childLsid, session->getSessionId());
    ASSERT(parentSession);
    ASSERT_EQ(parentLsid, parentSession->getSessionId());
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, CannotCheckOutParentSessionOfCheckedOutSession) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        _opCtx->setLogicalSessionId(childLsid);
        OperationContextSession ocs(_opCtx);

        // Verify that the parent session cannot be checked out until the child session is checked
        // back in.
        auto future = stdx::async(stdx::launch::async, [this, parentLsid] {
            ThreadClient tc(getServiceContext());
            auto opCtx = cc().makeOperationContext();
            opCtx->setLogicalSessionId(parentLsid);
            OperationContextSession ocs(opCtx.get());
        });
        ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

        OperationContextSession::checkIn(_opCtx, OperationContextSession::CheckInReason::kDone);
        ASSERT(!OperationContextSession::get(_opCtx));
        future.get();
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, CannotCheckOutChildSessionOfCheckedOutSession) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        _opCtx->setLogicalSessionId(parentLsid);
        OperationContextSession ocs(_opCtx);

        // Verify that the child session cannot be checked out until the parent session is checked
        // back in.
        auto future = stdx::async(stdx::launch::async, [this, childLsid] {
            ThreadClient tc(getServiceContext());
            auto opCtx = cc().makeOperationContext();
            opCtx->setLogicalSessionId(childLsid);
            OperationContextSession ocs(opCtx.get());
        });
        ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

        OperationContextSession::checkIn(_opCtx, OperationContextSession::CheckInReason::kDone);
        ASSERT(!OperationContextSession::get(_opCtx));
        future.get();
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, CannotCheckoutMultipleChildSessionsConcurrently) {
    auto runTest = [&](const LogicalSessionId& childLsid0, const LogicalSessionId& childLsid1) {
        _opCtx->setLogicalSessionId(childLsid0);
        OperationContextSession ocs(_opCtx);

        // Verify that another child session cannot be checked out until both the child session
        // above and the parent session are checked back in.
        auto future = stdx::async(stdx::launch::async, [this, childLsid1] {
            ThreadClient tc(getServiceContext());
            auto childSessionOpCtx1 = cc().makeOperationContext();
            childSessionOpCtx1->setLogicalSessionId(childLsid1);
            OperationContextSession ocs(childSessionOpCtx1.get());
        });
        ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

        OperationContextSession::checkIn(_opCtx, OperationContextSession::CheckInReason::kDone);
        ASSERT(!OperationContextSession::get(_opCtx));
        future.get();
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid),
            makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(makeLogicalSessionIdWithTxnUUIDForTest(parentLsid),
            makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid),
            makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
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
    auto runTest = [&](const LogicalSessionId& lsid) {
        _opCtx->setLogicalSessionId(lsid);

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
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
}

TEST_F(SessionCatalogTest, ScanSession) {
    // Create sessions in the catalog.
    const auto& lsids = []() -> std::vector<LogicalSessionId> {
        auto lsid0 = makeLogicalSessionIdForTest();
        auto lsid1 = makeLogicalSessionIdForTest();
        auto lsid2 = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(lsid1);
        auto lsid3 = makeLogicalSessionIdWithTxnUUIDForTest(lsid1);
        return {lsid0, lsid1, lsid2, lsid3};
    }();
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

    catalog()->scanSession(lsids[3], [&lsids](const ObservableSession& session) {
        ASSERT_EQ(lsids[3], session.get()->getSessionId());
    });

    catalog()->scanSession(makeLogicalSessionIdForTest(), [](const ObservableSession&) {
        FAIL("The callback was called for non-existent session");
    });
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, ScanSessionsForReapWhenSessionIsIdle) {
    auto parentLsid = makeLogicalSessionIdForTest();
    auto childLsid0 = makeLogicalSessionIdWithTxnUUIDForTest(parentLsid);
    auto childLsid1 = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid);
    auto otherParentLsid = makeLogicalSessionIdForTest();

    createSession(parentLsid);
    createSession(childLsid0);
    createSession(childLsid1);
    createSession(otherParentLsid);
    auto lsidsFound = getAllSessionIds(_opCtx);
    ASSERT_EQ(4U, lsidsFound.size());

    // Mark otherParentSession for reap. The session should get reaped since it doesn't have any
    // child session.
    auto lsidsNotReaped = catalog()->scanSessionsForReap(
        otherParentLsid,
        [](ObservableSession& parentSession) {
            parentSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
        },
        [](ObservableSession& childSession) {});
    lsidsFound = getAllSessionIds(_opCtx);
    ASSERT_EQ(3U, lsidsFound.size());
    catalog()->scanSession(otherParentLsid, [](const ObservableSession&) {
        FAIL("Found a session that should have been reaped");
    });
    ASSERT_EQ(0U, lsidsNotReaped.size());

    // Mark parentSession for reap. The session should not get reaped since its child sessions
    // (i.e. childSession0 and childSession1) are not marked for reaped.
    lsidsNotReaped = catalog()->scanSessionsForReap(
        parentLsid,
        [](ObservableSession& parentSession) {
            parentSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
        },
        [](ObservableSession& childSession) {});
    lsidsFound = getAllSessionIds(_opCtx);
    ASSERT_EQ(3U, lsidsFound.size());
    ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
    for (const auto& lsid : lsidsFound) {
        ASSERT(lsidsNotReaped.find(lsid) != lsidsNotReaped.end());
    }

    // Mark childSession0 and childSession1 for reap with kNonExclusive mode. The sessions should
    // not get reaped since parentSession is not marked for reaped.
    lsidsNotReaped = catalog()->scanSessionsForReap(
        parentLsid,
        [](ObservableSession& parentSession) {

        },
        [](ObservableSession& childSession) {
            childSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
        });
    lsidsFound = getAllSessionIds(_opCtx);
    ASSERT_EQ(3U, lsidsFound.size());
    ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
    for (const auto& lsid : lsidsFound) {
        ASSERT(lsidsNotReaped.find(lsid) != lsidsNotReaped.end());
    }

    // Mark childSession0 for reap with kExclusive mode. The session should get reaped.
    lsidsNotReaped = catalog()->scanSessionsForReap(
        parentLsid,
        [](ObservableSession& parentSession) {},
        [&](ObservableSession& childSession) {
            if (childSession.getSessionId() == childLsid0) {
                childSession.markForReap(ObservableSession::ReapMode::kExclusive);
            }
        });
    lsidsFound = getAllSessionIds(_opCtx);
    ASSERT_EQ(2U, lsidsFound.size());
    catalog()->scanSession(childLsid0, [](const ObservableSession&) {
        FAIL("Found a session that should have been reaped");
    });
    ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
    for (const auto& lsid : lsidsFound) {
        ASSERT(lsidsNotReaped.find(lsid) != lsidsNotReaped.end());
    }

    // Mark parentSession and childSession1 for reap with kNonExclusive mode. Both sessions should
    // get reaped since all sessions are now marked for reap.
    lsidsNotReaped = catalog()->scanSessionsForReap(
        parentLsid,
        [](ObservableSession& parentSession) {
            parentSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
        },
        [](ObservableSession& childSession) {
            childSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
        });

    lsidsFound = getAllSessionIds(_opCtx);
    ASSERT_EQ(0U, lsidsFound.size());
    ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
}

DEATH_TEST_F(SessionCatalogTestWithDefaultOpCtx,
             ScanSessionDoesNotSupportReaping,
             "Cannot reap a session via 'scanSession'") {
    auto lsid = makeLogicalSessionIdForTest();

    {
        _opCtx->setLogicalSessionId(lsid);
        OperationContextSession ocs(_opCtx);
    }

    catalog()->scanSession(lsid, [](ObservableSession& session) {
        session.markForReap(ObservableSession::ReapMode::kNonExclusive);
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

    // Create sessions in the catalog.
    const auto& lsids = []() -> std::vector<LogicalSessionId> {
        auto lsid0 = makeLogicalSessionIdForTest();
        auto lsid1 = makeLogicalSessionIdForTest();
        auto lsid2 = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(lsid1);
        auto lsid3 = makeLogicalSessionIdWithTxnUUIDForTest(lsid1);
        return {lsid0, lsid1, lsid2, lsid3};
    }();
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
    ASSERT_EQ(4U, lsidsFound.size());
    lsidsFound.clear();

    // Scan over all Sessions, visiting a Session with child Sessions.
    SessionKiller::Matcher matcherLSID1(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx, lsids[1])});
    catalog()->scanSessions(matcherLSID1, workerFn);

    ASSERT_EQ(3U, lsidsFound.size());

    const auto searchLsidsFound = [&](const LogicalSessionId lsid) {
        return std::find(lsidsFound.begin(), lsidsFound.end(), lsid) != lsidsFound.end();
    };

    for (size_t i = 1; i < lsids.size(); ++i) {
        if (!searchLsidsFound(lsids[i])) {
            FAIL("Match missed an lsid");
        }
    }
    lsidsFound.clear();

    // Do not allow matching on child sessions.
    ASSERT_THROWS_CODE(KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx, lsids[2])},
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, ScanSessionsForReapWhenParentSessionIsCheckedOut) {
    auto runTest = [&](bool hangAfterIncrementingNumWaitingToCheckOut) {
        auto parentLsid = makeLogicalSessionIdForTest();
        auto childLsid0 = makeLogicalSessionIdWithTxnUUIDForTest(parentLsid);
        auto childLsid1 = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid);

        createSession(parentLsid);
        createSession(childLsid0);
        createSession(childLsid1);
        auto lsidsFound = getAllSessionIds(_opCtx);
        ASSERT_EQ(3U, lsidsFound.size());

        unittest::Barrier sessionsCheckedOut(2);
        unittest::Barrier sessionsCheckedIn(2);

        // Check out parentSession.
        auto future = stdx::async(stdx::launch::async, [&] {
            ThreadClient tc(getServiceContext());
            auto opCtx = makeOperationContext();

            if (hangAfterIncrementingNumWaitingToCheckOut) {
                auto fp =
                    globalFailPointRegistry().find("hangAfterIncrementingNumWaitingToCheckOut");
                auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

                auto innerFuture = stdx::async(stdx::launch::async, [&] {
                    ThreadClient innerTc(getServiceContext());
                    auto innerOpCtx = makeOperationContext();
                    innerOpCtx->setLogicalSessionId(parentLsid);
                    OperationContextSession ocs(innerOpCtx.get());
                });

                fp->waitForTimesEntered(opCtx.get(), initialTimesEntered + 1);
                sessionsCheckedOut.countDownAndWait();
                sessionsCheckedIn.countDownAndWait();
                fp->setMode(FailPoint::off);
                innerFuture.get();
            } else {
                opCtx->setLogicalSessionId(parentLsid);
                OperationContextSession ocs(opCtx.get());
                sessionsCheckedOut.countDownAndWait();
                sessionsCheckedIn.countDownAndWait();
            }
        });
        // After this wait, parentSession is either checked out or has a thread waiting for it be
        // checked out.
        sessionsCheckedOut.countDownAndWait();

        // Mark parentSession for reap, and additionally mark childSession0 and childSession1 for
        // reap with kNonExclusive mode. parentSession should not get reaped because it is checked
        // out or has a thread waiting for it be checked out. childSession0 and childSession1 also
        // should not get reaped since they must be reaped with parentSession.
        auto lsidsNotReaped = catalog()->scanSessionsForReap(
            parentLsid,
            [](ObservableSession& parentSession) {
                parentSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
            },
            [](ObservableSession& childSession) {
                childSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
            });
        lsidsFound = getAllSessionIds(_opCtx);
        ASSERT_EQ(3U, lsidsFound.size());
        ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
        for (const auto& lsid : lsidsFound) {
            ASSERT(lsidsNotReaped.find(lsid) != lsidsNotReaped.end());
        }

        // Mark childSession0 for reap with kExclusive mode. It should get reaped although
        // parentSession is checked out or has a thread waiting for it be checked out.
        lsidsNotReaped = catalog()->scanSessionsForReap(
            parentLsid,
            [](ObservableSession& parentSession) {},
            [&](ObservableSession& childSession) {
                if (childSession.getSessionId() == childLsid0) {
                    childSession.markForReap(ObservableSession::ReapMode::kExclusive);
                }
            });
        lsidsFound = getAllSessionIds(_opCtx);
        ASSERT_EQ(2U, lsidsFound.size());
        catalog()->scanSession(childLsid0, [](const ObservableSession&) {
            FAIL("Found a session that should have been reaped");
        });
        ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
        for (const auto& lsid : lsidsFound) {
            ASSERT(lsidsNotReaped.find(lsid) != lsidsNotReaped.end());
        }

        // Mark childSession1 for reap with mode kExclusive. The session should get reaped although
        // parentSession is checked out or has a thread waiting for it be checked out.
        lsidsNotReaped = catalog()->scanSessionsForReap(
            parentLsid,
            [](ObservableSession& parentSession) {},
            [](ObservableSession& childSession) {
                childSession.markForReap(ObservableSession::ReapMode::kExclusive);
            });
        lsidsFound = getAllSessionIds(_opCtx);
        ASSERT_EQ(1U, lsidsFound.size());
        catalog()->scanSession(childLsid1, [](const ObservableSession&) {
            FAIL("Found a session that should have been reaped");
        });
        ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
        for (const auto& lsid : lsidsFound) {
            ASSERT(lsidsNotReaped.find(lsid) != lsidsNotReaped.end());
        }

        // After this point, parentSession is checked back in.
        sessionsCheckedIn.countDownAndWait();
        future.get();

        // Mark parentSession for reap. The session should now get reaped.
        lsidsNotReaped = catalog()->scanSessionsForReap(
            parentLsid,
            [](ObservableSession& parentSession) {
                parentSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
            },
            [](ObservableSession& childSession) {

            });

        lsidsFound = getAllSessionIds(_opCtx);
        ASSERT_EQ(0U, lsidsFound.size());
        ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
    };

    runTest(false /* hangAfterIncrementingNumWaitingToCheckOut */);
    runTest(true /* hangAfterIncrementingNumWaitingToCheckOut */);
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, ScanSessionsForReapWhenChildSessionIsCheckedOut) {
    auto runTest = [&](bool hangAfterIncrementingNumWaitingToCheckOut) {
        auto parentLsid = makeLogicalSessionIdForTest();
        auto parentTxnNumber = TxnNumber{0};
        auto childLsid0 = makeLogicalSessionIdWithTxnUUIDForTest(parentLsid);
        auto childLsid1 =
            makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber++);
        auto childLsid2 =
            makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);

        createSession(parentLsid);
        createSession(childLsid0);
        createSession(childLsid1);
        createSession(childLsid2);
        auto lsidsFound = getAllSessionIds(_opCtx);
        ASSERT_EQ(4U, lsidsFound.size());

        unittest::Barrier sessionsCheckedOut(2);
        unittest::Barrier sessionsCheckedIn(2);

        // Check out childSession2.
        auto future = stdx::async(stdx::launch::async, [&] {
            ThreadClient tc(getServiceContext());
            auto opCtx = makeOperationContext();

            if (hangAfterIncrementingNumWaitingToCheckOut) {
                auto fp =
                    globalFailPointRegistry().find("hangAfterIncrementingNumWaitingToCheckOut");
                auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

                auto innerFuture = stdx::async(stdx::launch::async, [&] {
                    ThreadClient innerTc(getServiceContext());
                    auto innerOpCtx = makeOperationContext();
                    innerOpCtx->setLogicalSessionId(childLsid2);
                    OperationContextSession ocs(innerOpCtx.get());
                });

                fp->waitForTimesEntered(opCtx.get(), initialTimesEntered + 1);
                sessionsCheckedOut.countDownAndWait();
                sessionsCheckedIn.countDownAndWait();
                fp->setMode(FailPoint::off);
                innerFuture.get();
            } else {
                opCtx->setLogicalSessionId(childLsid2);
                OperationContextSession ocs(opCtx.get());
                sessionsCheckedOut.countDownAndWait();
                sessionsCheckedIn.countDownAndWait();
            }
        });
        // After this wait, childSession2 is either checked out or has a thread waiting for it be
        // checked out.
        sessionsCheckedOut.countDownAndWait();

        // Mark childSession2 for reap with kExclusive mode. The session should not get reaped since
        // it is checked out or has a thread waiting for it be checked out.
        auto lsidsNotReaped = catalog()->scanSessionsForReap(
            parentLsid,
            [](ObservableSession& parentSession) {},
            [&](ObservableSession& childSession) {
                if (childSession.getSessionId() == childLsid2) {
                    childSession.markForReap(ObservableSession::ReapMode::kExclusive);
                }
            });
        lsidsFound = getAllSessionIds(_opCtx);
        ASSERT_EQ(4U, lsidsFound.size());
        ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
        for (const auto& lsid : lsidsFound) {
            ASSERT(lsidsNotReaped.find(lsid) != lsidsNotReaped.end());
        }

        // Mark parentSession for reap, and additionally mark Reap childSession0 and childSession1
        // with mode kExclusive. parentSession should not get reaped because childSession2 is
        // checked out or has a thread waiting for it be checked out. childSession0 and
        // childSession1 should get reaped since they are not checked out or have any threads
        // waiting for them be checked out.
        lsidsNotReaped = catalog()->scanSessionsForReap(
            parentLsid,
            [&](ObservableSession& parentSession) {
                parentSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
            },
            [&](ObservableSession& childSession) {
                auto lsid = childSession.getSessionId();
                if (lsid == childLsid0 || lsid == childLsid1) {
                    childSession.markForReap(ObservableSession::ReapMode::kExclusive);
                }
            });

        lsidsFound = getAllSessionIds(_opCtx);
        ASSERT_EQ(2U, lsidsFound.size());
        catalog()->scanSession(childLsid0, [](const ObservableSession&) {
            FAIL("Found a session that should have been reaped");
        });
        catalog()->scanSession(childLsid1, [](const ObservableSession&) {
            FAIL("Found a session that should have been reaped");
        });
        ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
        for (const auto& lsid : lsidsFound) {
            ASSERT(lsidsNotReaped.find(lsid) != lsidsNotReaped.end());
        }

        // After this point, childSession2 is checked back in.
        sessionsCheckedIn.countDownAndWait();
        future.get();

        // Mark parentSession and childSession2 for reap with kNonExclusive mode. Both sessions
        // should get reaped.
        lsidsNotReaped = catalog()->scanSessionsForReap(
            parentLsid,
            [](ObservableSession& parentSession) {
                parentSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
            },
            [](ObservableSession& childSession) {
                childSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
            });

        lsidsFound = getAllSessionIds(_opCtx);
        ASSERT_EQ(0U, lsidsFound.size());
        ASSERT_EQ(lsidsFound.size(), lsidsNotReaped.size());
    };

    runTest(false /* hangAfterIncrementingNumWaitingToCheckOut */);
    runTest(true /* hangAfterIncrementingNumWaitingToCheckOut */);
}

DEATH_TEST_F(SessionCatalogTestWithDefaultOpCtx,
             ScanSessionsDoesNotSupportReaping,
             "Cannot reap a session via 'scanSessions'") {
    {
        auto lsid = makeLogicalSessionIdForTest();
        _opCtx->setLogicalSessionId(lsid);
        OperationContextSession ocs(_opCtx);
    }

    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx)});
    catalog()->scanSessions(matcherAllSessions, [](ObservableSession& session) {
        session.markForReap(ObservableSession::ReapMode::kNonExclusive);
    });
}

TEST_F(SessionCatalogTest, KillSessionWhenSessionIsNotCheckedOut) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        // Create the session so there is something to kill
        {
            auto opCtx = makeOperationContext();
            opCtx->setLogicalSessionId(lsid);
            OperationContextSession ocs(opCtx.get());
        }

        auto killToken = catalog()->killSession(lsid);

        // Make sure that regular session check-out will fail because the session is marked as
        // killed
        assertSessionCheckoutTimesOut(lsid);

        // Schedule a separate "regular operation" thread, which will block on checking-out the
        // session, which we will use to confirm that session kill completion actually unblocks
        // check-out
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
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
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
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
}

TEST_F(SessionCatalogTest, KillSessionWhenSessionIsCheckedOut) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        auto killToken = [this, &lsid] {
            // Create the session so there is something to kill
            auto opCtx = makeOperationContext();
            opCtx->setLogicalSessionId(lsid);
            OperationContextSession operationContextSession(opCtx.get());

            auto killToken = catalog()->killSession(lsid);

            // Make sure the owning operation context is interrupted
            ASSERT_THROWS_CODE(
                opCtx->checkForInterrupt(), AssertionException, ErrorCodes::Interrupted);

            // Make sure that the checkOutForKill call will wait for the owning operation context to
            // check the session back in
            assertConcurrentCheckoutTimesOut(lsid);

            return killToken;
        }();

        // Make sure that regular session check-out will fail because the session is marked as
        // killed
        assertSessionCheckoutTimesOut(lsid);

        // Schedule a separate "regular operation" thread, which will block on checking-out the
        // session, which we will use to confirm that session kill completion actually unblocks
        // check-out
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
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
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
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
}

TEST_F(SessionCatalogTest, KillParentSessionWhenChildSessionIsCheckedOut) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        auto killToken = [this, &parentLsid, &childLsid] {
            // Create the session so there is something to kill
            auto opCtx = makeOperationContext();
            opCtx->setLogicalSessionId(childLsid);
            OperationContextSession operationContextSession(opCtx.get());

            auto killToken = catalog()->killSession(parentLsid);

            // Make sure the owning operation context is interrupted
            ASSERT_THROWS_CODE(
                opCtx->checkForInterrupt(), AssertionException, ErrorCodes::Interrupted);

            // Make sure that the checkOutForKill call will wait for the owning operation context to
            // check the session back in
            assertConcurrentCheckoutTimesOut(childLsid);

            return killToken;
        }();

        // Make sure that regular session check-out will fail because the session is marked as
        // killed
        assertSessionCheckoutTimesOut(childLsid);

        // Schedule a separate "regular operation" thread, which will block on checking-out the
        // session, which we will use to confirm that session kill completion actually unblocks
        // check-out
        auto future = stdx::async(stdx::launch::async, [this, childLsid] {
            ThreadClient tc(getServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(childLsid);
            OperationContextSession ocs(sideOpCtx.get());
        });
        ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

        // Make sure that "for kill" session check-out succeeds
        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }

        // Make sure that session check-out after kill succeeds again
        {
            auto opCtx = makeOperationContext();
            opCtx->setLogicalSessionId(childLsid);
            OperationContextSession ocs(opCtx.get());
        }

        // Make sure the "regular operation" eventually is able to proceed and use the just killed
        // session
        future.get();
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

TEST_F(SessionCatalogTest, KillParentSessionWhenChildSessionIsNotCheckedOut) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        auto killToken = [this, &parentLsid, &childLsid] {
            // Create the session so there is something to kill
            {
                auto opCtx = makeOperationContext();
                opCtx->setLogicalSessionId(childLsid);
                OperationContextSession operationContextSession(opCtx.get());
            }

            auto killToken = catalog()->killSession(parentLsid);

            // Make sure that the checkOutForKill call will wait for the owning operation context to
            // check the session back in
            assertConcurrentCheckoutTimesOut(childLsid);

            return killToken;
        }();

        // Make sure that regular session check-out will fail because the session is marked as
        // killed
        assertSessionCheckoutTimesOut(childLsid);

        // Schedule a separate "regular operation" thread, which will block on checking-out the
        // session, which we will use to confirm that session kill completion actually unblocks
        // check-out
        auto future = stdx::async(stdx::launch::async, [this, childLsid] {
            ThreadClient tc(getServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(childLsid);
            OperationContextSession ocs(sideOpCtx.get());
        });
        ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

        // Make sure that "for kill" session check-out succeeds
        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }

        // Make sure that session check-out after kill succeeds again
        {
            auto opCtx = makeOperationContext();
            opCtx->setLogicalSessionId(childLsid);
            OperationContextSession ocs(opCtx.get());
        }

        // Make sure the "regular operation" eventually is able to proceed and use the just killed
        // session
        future.get();
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

TEST_F(SessionCatalogTest, KillSessionWhenChildSessionIsCheckedOut) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        auto killToken = [this, &parentLsid, &childLsid] {
            // Create the session so there is something to kill
            auto opCtx = makeOperationContext();
            opCtx->setLogicalSessionId(childLsid);
            OperationContextSession operationContextSession(opCtx.get());

            auto killToken = catalog()->killSession(childLsid);

            // Make sure the owning operation context is interrupted
            ASSERT_THROWS_CODE(
                opCtx->checkForInterrupt(), AssertionException, ErrorCodes::Interrupted);

            // Make sure that the checkOutForKill call will wait for the owning operation context to
            // check the session back in
            assertConcurrentCheckoutTimesOut(parentLsid);
            assertConcurrentCheckoutTimesOut(childLsid);

            return killToken;
        }();

        // Make sure that regular session check-out will fail because the session is marked as
        // killed
        assertSessionCheckoutTimesOut(childLsid);

        // Check that checking out the parent session will fail because it was marked as killed.
        assertSessionCheckoutTimesOut(parentLsid);

        // Schedule a separate "regular operation" thread, which will block on checking-out the
        // session, which we will use to confirm that session kill completion actually unblocks
        // check-out
        auto childFuture = stdx::async(stdx::launch::async, [this, childLsid] {
            ThreadClient tc(getServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(childLsid);
            OperationContextSession ocs(sideOpCtx.get());
        });
        ASSERT(stdx::future_status::ready !=
               childFuture.wait_for(Milliseconds(10).toSystemDuration()));

        auto parentFuture = stdx::async(stdx::launch::async, [this, parentLsid] {
            ThreadClient tc(getServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(parentLsid);
            OperationContextSession ocs(sideOpCtx.get());
        });
        ASSERT(stdx::future_status::ready !=
               parentFuture.wait_for(Milliseconds(10).toSystemDuration()));

        // Make sure that "for kill" session check-out succeeds
        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }

        // Make sure that session check-out after kill succeeds again
        assertCanCheckoutSession(childLsid);
        assertCanCheckoutSession(parentLsid);

        // Make sure the "regular operations" eventually proceed and use the just killed session
        childFuture.get();
        parentFuture.get();
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

TEST_F(SessionCatalogTest, KillSessionWhenChildSessionIsNotCheckedOut) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        auto killToken = [this, &parentLsid, &childLsid] {
            // Create the session so there is something to kill
            {
                auto opCtx = makeOperationContext();
                opCtx->setLogicalSessionId(childLsid);
                OperationContextSession ocs(opCtx.get());
            }

            auto killToken = catalog()->killSession(childLsid);

            // Make sure that the checkOutForKill call will wait for the owning operation context to
            // check the session back in
            assertConcurrentCheckoutTimesOut(parentLsid);
            assertConcurrentCheckoutTimesOut(childLsid);

            return killToken;
        }();

        // Make sure that regular session check-out will fail because the session is marked as
        // killed
        assertSessionCheckoutTimesOut(childLsid);

        // Check that checking out the parent session will fail because it was marked as killed.
        assertSessionCheckoutTimesOut(parentLsid);

        // Schedule a separate "regular operation" thread, which will block on checking-out the
        // session, which we will use to confirm that session kill completion actually unblocks
        // check-out
        auto childFuture = stdx::async(stdx::launch::async, [this, childLsid] {
            ThreadClient tc(getServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(childLsid);
            OperationContextSession ocs(sideOpCtx.get());
        });
        ASSERT(stdx::future_status::ready !=
               childFuture.wait_for(Milliseconds(10).toSystemDuration()));

        auto parentFuture = stdx::async(stdx::launch::async, [this, parentLsid] {
            ThreadClient tc(getServiceContext());
            auto sideOpCtx = Client::getCurrent()->makeOperationContext();
            sideOpCtx->setLogicalSessionId(parentLsid);
            OperationContextSession ocs(sideOpCtx.get());
        });
        ASSERT(stdx::future_status::ready !=
               parentFuture.wait_for(Milliseconds(10).toSystemDuration()));

        // Make sure that "for kill" session check-out succeeds
        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }

        // Make sure that session check-out after kill succeeds again
        assertCanCheckoutSession(childLsid);
        assertCanCheckoutSession(parentLsid);

        // Make sure the "regular operations" eventually proceed and use the just killed session
        childFuture.get();
        parentFuture.get();
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

TEST_F(SessionCatalogTest, KillingChildSessionDoesNotInterruptParentSession) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        auto killToken = [this, &parentLsid, &childLsid] {
            assertCanCheckoutSession(childLsid);

            auto opCtx = makeOperationContext();
            opCtx->setLogicalSessionId(parentLsid);
            OperationContextSession operationContextSession(opCtx.get());

            auto killToken = catalog()->killSession(childLsid);

            // Make sure the owning operation context is not interrupted.
            opCtx->checkForInterrupt();

            // Make sure that the checkOutForKill call will wait for the owning operation context to
            // check the session back in
            assertConcurrentCheckoutTimesOut(parentLsid);
            assertConcurrentCheckoutTimesOut(childLsid);

            return killToken;
        }();

        // Use up the killToken.
        auto opCtx = makeOperationContext();
        auto scopedSession = catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken));
        ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

TEST_F(SessionCatalogTest, KillParentThenChildSession) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        // Create a child session, which will also make the parent session.
        assertCanCheckoutSession(childLsid);

        // Kill the parent.
        auto parentKillToken = catalog()->killSession(parentLsid);

        // Verify we can't check out either child or parent now.
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        // We can still kill the child.
        auto childKillToken = catalog()->killSession(childLsid);

        // Verify we can't check back out either session until all kill tokens have been used.
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(parentKillToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(childKillToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        assertCanCheckoutSession(childLsid);
        assertCanCheckoutSession(parentLsid);
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

TEST_F(SessionCatalogTest, KillChildThenParentSession) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        // Create a child session, which will also make the parent session.
        assertCanCheckoutSession(childLsid);

        // Kill the child.
        auto childKillToken = catalog()->killSession(childLsid);

        // Verify we can't check out either child or parent now.
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        // We can still kill the parent.
        auto parentKillToken = catalog()->killSession(parentLsid);

        // Verify we can't check back out either session until all kill tokens have been used.
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(childKillToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(parentKillToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        assertCanCheckoutSession(childLsid);
        assertCanCheckoutSession(parentLsid);
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}

TEST_F(SessionCatalogTest, KillChildAndParentMultipleTimes) {
    auto runTest = [&](const LogicalSessionId& parentLsid, const LogicalSessionId& childLsid) {
        // Create a child session, which will also make the parent session.
        assertCanCheckoutSession(childLsid);

        // Kill the parent and child in interleaved orders to verify the kill order doesn't matter.
        auto firstParentKillToken = catalog()->killSession(parentLsid);

        auto firstChildKillToken = catalog()->killSession(childLsid);
        auto secondChildKillToken = catalog()->killSession(childLsid);

        auto secondParentKillToken = catalog()->killSession(parentLsid);

        auto thirdChildKillToken = catalog()->killSession(childLsid);

        // Verify we can't check back out either session until all kill tokens have been used. Use
        // an arbitrary order to verify the return order doesn't matter.
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(firstChildKillToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(secondParentKillToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(thirdChildKillToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(firstParentKillToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        assertSessionCheckoutTimesOut(childLsid);
        assertSessionCheckoutTimesOut(parentLsid);

        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(secondChildKillToken));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        assertCanCheckoutSession(childLsid);
        assertCanCheckoutSession(parentLsid);
    };

    auto parentLsid = makeLogicalSessionIdForTest();
    runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
}


TEST_F(SessionCatalogTest, MarkSessionAsKilledCanBeCalledMoreThanOnce) {
    auto runTest = [&](const LogicalSessionId& lsid) {
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
        assertSessionCheckoutTimesOut(lsid);

        boost::optional<SessionCatalog::KillToken> killTokenWhileSessionIsCheckedOutForKill;

        // Finish the first killer of the session
        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken1));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());

            // Killing a session while checked out for kill should not affect the killers
            killTokenWhileSessionIsCheckedOutForKill.emplace(catalog()->killSession(lsid));
        }

        // Regular session check-out should still fail because there are now still two killers on
        // the session
        assertSessionCheckoutTimesOut(lsid);
        {
            auto opCtx = makeOperationContext();
            auto scopedSession =
                catalog()->checkOutSessionForKill(opCtx.get(), std::move(killToken2));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
        {
            auto opCtx = makeOperationContext();
            auto scopedSession = catalog()->checkOutSessionForKill(
                opCtx.get(), std::move(*killTokenWhileSessionIsCheckedOutForKill));
            ASSERT_EQ(opCtx.get(), scopedSession.currentOperation_forTest());
        }
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
}

TEST_F(SessionCatalogTest, MarkNonExistentSessionAsKilled) {
    auto runTest = [&](const LogicalSessionId& nonExistentLsid) {
        ASSERT_THROWS_CODE(
            catalog()->killSession(nonExistentLsid), AssertionException, ErrorCodes::NoSuchSession);
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
}

TEST_F(SessionCatalogTest, MarkNonExistentChildSessionAsKilledWhenParentSessionExists) {
    auto runTest = [&](const LogicalSessionId& parentLsid,
                       const LogicalSessionId& nonExistentChildLsid) {
        createSession(parentLsid);
        ASSERT_THROWS_CODE(catalog()->killSession(nonExistentChildLsid),
                           AssertionException,
                           ErrorCodes::NoSuchSession);
    };

    {
        auto parentLsid = makeLogicalSessionIdForTest();
        runTest(parentLsid, makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    }

    {
        auto parentLsid = makeLogicalSessionIdForTest();
        runTest(parentLsid, makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
    }
}


TEST_F(SessionCatalogTest, MarkNonExistentChildSessionAsKilledWhenOtherChildSessionExists) {
    auto runTest = [&](const LogicalSessionId& existentChildLsid,
                       const LogicalSessionId& nonExistentChildLsid) {
        createSession(existentChildLsid);
        ASSERT_THROWS_CODE(catalog()->killSession(nonExistentChildLsid),
                           AssertionException,
                           ErrorCodes::NoSuchSession);
    };

    {
        auto parentLsid = makeLogicalSessionIdForTest();
        runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid),
                makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    }

    {
        auto parentLsid = makeLogicalSessionIdForTest();
        runTest(makeLogicalSessionIdWithTxnUUIDForTest(parentLsid),
                makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
    }

    {
        auto parentLsid = makeLogicalSessionIdForTest();
        runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid),
                makeLogicalSessionIdWithTxnUUIDForTest(parentLsid));
    }

    {
        auto parentLsid = makeLogicalSessionIdForTest();
        runTest(makeLogicalSessionIdWithTxnUUIDForTest(parentLsid),
                makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid));
    }
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, SessionDiscarOperationContextAfterCheckIn) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        _opCtx->setLogicalSessionId(lsid);

        {
            OperationContextSession ocs(_opCtx);
            ASSERT(OperationContextSession::get(_opCtx));

            OperationContextSession::checkIn(_opCtx, OperationContextSession::CheckInReason::kDone);
            ASSERT(!OperationContextSession::get(_opCtx));
        }

        ASSERT(!OperationContextSession::get(_opCtx));
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, SessionDiscarOperationContextAfterCheckInCheckOut) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        _opCtx->setLogicalSessionId(lsid);

        {
            OperationContextSession ocs(_opCtx);
            ASSERT(OperationContextSession::get(_opCtx));

            OperationContextSession::checkIn(_opCtx, OperationContextSession::CheckInReason::kDone);
            ASSERT(!OperationContextSession::get(_opCtx));

            OperationContextSession::checkOut(_opCtx);
            ASSERT(OperationContextSession::get(_opCtx));
        }

        ASSERT(!OperationContextSession::get(_opCtx));
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, KillSessionsThroughScanSessions) {
    // Create sessions in the catalog.
    const auto lsids = []() -> std::vector<LogicalSessionId> {
        auto lsid0 = makeLogicalSessionIdForTest();
        auto lsid1 = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
        auto lsid2 = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
        return {lsid0, lsid1, lsid2};
    }();

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
    auto runTest = [&](const LogicalSessionId& lsid) {
        auto client = getServiceContext()->makeClient("ConcurrentCheckOutAndKill");
        AlternativeClientRegion acr(client);
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(lsid);

        stdx::future<void> normalCheckOutFinish, killCheckOutFinish;

        // This variable is protected by the session check-out.
        std::string lastSessionCheckOut = "first session";
        {
            // Check out the session to block both normal check-out and checkOutForKill.
            OperationContextSession firstCheckOut(opCtx.get());

            // Normal check out should start after kill.
            normalCheckOutFinish = stdx::async(stdx::launch::async, [&] {
                ThreadClient tc(getServiceContext());
                auto sideOpCtx = Client::getCurrent()->makeOperationContext();
                sideOpCtx->setLogicalSessionId(lsid);
                OperationContextSession normalCheckOut(sideOpCtx.get());
                ASSERT_EQ("session kill", lastSessionCheckOut);
                lastSessionCheckOut = "session checkout";
            });

            // Kill will short-cut the queue and be the next one to check out.
            killCheckOutFinish = stdx::async(stdx::launch::async, [&] {
                ThreadClient tc(getServiceContext());
                auto sideOpCtx = Client::getCurrent()->makeOperationContext();
                sideOpCtx->setLogicalSessionId(lsid);

                // Kill the session
                std::vector<SessionCatalog::KillToken> killTokens;
                catalog()->scanSession(lsid, [&killTokens](const ObservableSession& session) {
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
                auto m = MONGO_MAKE_LATCH();
                stdx::condition_variable cond;
                stdx::unique_lock<Latch> lock(m);
                ASSERT_THROWS_CODE(
                    opCtx->waitForConditionOrInterrupt(cond, lock, [] { return false; }),
                    DBException,
                    ErrorCodes::InternalError);
            }
        }
        normalCheckOutFinish.get();
        killCheckOutFinish.get();

        ASSERT_EQ("session checkout", lastSessionCheckOut);
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
}

}  // namespace
}  // namespace mongo

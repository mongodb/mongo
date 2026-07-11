// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/kill_sessions_local.h"

#include "mongo/db/session/session_catalog_test.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

#include <future>
#include <memory>
#include <string_view>

namespace mongo {
namespace {
class KillSessionsTest : public SessionCatalogTest {
protected:
    void expireAllTransactions(OperationContext* opCtx) {
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});

        const auto getAllSessionIdsWorkerFn = [&opCtx, this](const ObservableSession& session) {
            auto txnParticipant = TransactionParticipant::get(opCtx, session.get());
            txnParticipant.transitionToInProgressForTest();
            txnParticipant.setTransactionExpiredDate(Date_t::now() - Milliseconds(1));
            // Timer has to be started in order to update metrics in the transactionMetricsObserver
            // as a part of aborting transactions.
            advanceTransactionMetricsTimer(opCtx, txnParticipant);
        };

        catalog()->scanSessions(matcherAllSessions, getAllSessionIdsWorkerFn);
    };

    void advanceTransactionMetricsTimer(OperationContext* opCtx,
                                        TransactionParticipant::Participant txnParticipant) {
        auto tickSourceMock = std::make_unique<TickSourceMock<Microseconds>>();
        tickSourceMock->advance(Milliseconds{100});
        txnParticipant.startMetricsTimer(opCtx, tickSourceMock.get(), Date_t::now(), Date_t::now());
    }
};

TEST_F(KillSessionsTest, killAllExpiredTransactionsSuccessfully) {
    auto lsid = makeLogicalSessionIdForTest();
    createSession(lsid);
    auto opCtx = makeOperationContext();
    opCtx->setLogicalSessionId(lsid);

    {
        // Checkout sessions in order to update transaction expiration.
        OperationContextSession firstCheckOut(opCtx.get());
        expireAllTransactions(opCtx.get());
    }

    auto client = getServiceContext()->getService()->makeClient("CheckOutForKill");
    AlternativeClientRegion acr(client);
    auto sideOpCtx = cc().makeOperationContext();
    sideOpCtx->setLogicalSessionId(lsid);

    int64_t numKills = 0;
    int64_t numTimeOuts = 0;

    killAllExpiredTransactions(sideOpCtx.get(), Milliseconds(100), &numKills, &numTimeOuts);

    ASSERT_EQ(1, numKills);
    ASSERT_EQ(0, numTimeOuts);
}

TEST_F(KillSessionsTest, killAllExpiredTransactionsTimesOut) {
    auto lsid = makeLogicalSessionIdForTest();
    createSession(lsid);
    auto opCtx = makeOperationContext();
    opCtx->setLogicalSessionId(lsid);

    // Checkout sessions in order to update transaction expiration. Session needs to stay checked
    // out in order to block kill and timeout.
    OperationContextSession firstCheckOut(opCtx.get());
    expireAllTransactions(opCtx.get());

    auto client = getServiceContext()->getService()->makeClient("CheckOutForKillTimeout");
    AlternativeClientRegion acr(client);
    auto sideOpCtx = cc().makeOperationContext();
    sideOpCtx->setLogicalSessionId(lsid);

    int64_t numKills = 0;
    int64_t numTimeOuts = 0;

    killAllExpiredTransactions(sideOpCtx.get(), Milliseconds(0), &numKills, &numTimeOuts);

    ASSERT_EQ(0, numKills);
    ASSERT_EQ(1, numTimeOuts);
}

TEST_F(KillSessionsTest, killOldestTransaction) {
    auto lsid = makeLogicalSessionIdForTest();

    createSession(lsid);
    auto opCtx = makeOperationContext();
    opCtx->setLogicalSessionId(lsid);

    {
        // Checkout sessions in order to update transaction expiration.
        OperationContextSession firstCheckOut(opCtx.get());
        expireAllTransactions(opCtx.get());
    }

    auto client = getServiceContext()->getService()->makeClient("CheckOutForKillOldestTransaction");
    AlternativeClientRegion acr(client);
    auto sideOpCtx = cc().makeOperationContext();
    sideOpCtx->setLogicalSessionId(lsid);

    int64_t numKills = 0;
    int64_t numSkips = 0;
    int64_t numTimeOuts = 0;

    killOldestTransaction(sideOpCtx.get(), Milliseconds(100), &numKills, &numSkips, &numTimeOuts);

    ASSERT_EQ(1, numKills);
    ASSERT_EQ(0, numSkips);
    ASSERT_EQ(0, numTimeOuts);
}

TEST_F(KillSessionsTest, killOldestTransactionSkipsInternalSession) {
    auto lsid = makeLogicalSessionIdWithTxnUUIDForTest();

    createSession(lsid);
    auto opCtx = makeOperationContext();
    opCtx->setLogicalSessionId(lsid);

    // Internal sessions are created as child sessions.
    ASSERT_EQ(2, getAllSessionIds(opCtx.get()).size());

    {
        // Checkout sessions in order to update transaction expiration.
        OperationContextSession firstCheckOut(opCtx.get());
        expireAllTransactions(opCtx.get());
    }

    auto client =
        getServiceContext()->getService()->makeClient("CheckOutForKillOldestInternalTransaction");
    AlternativeClientRegion acr(client);
    auto sideOpCtx = cc().makeOperationContext();
    sideOpCtx->setLogicalSessionId(lsid);

    int64_t numKills = 0;
    int64_t numSkips = 0;
    int64_t numTimeOuts = 0;

    killOldestTransaction(sideOpCtx.get(), Milliseconds(100), &numKills, &numSkips, &numTimeOuts);

    ASSERT_EQ(1, numKills);
    ASSERT_EQ(1, numSkips);
    ASSERT_EQ(0, numTimeOuts);
}

TEST_F(KillSessionsTest, killOldestTransactionTimesOut) {
    auto lsid = makeLogicalSessionIdForTest();

    createSession(lsid);
    auto opCtx = makeOperationContext();
    opCtx->setLogicalSessionId(lsid);

    // Checkout sessions in order to update transaction expiration.  Session needs to stay checked
    // out in order to block kill and timeout.
    OperationContextSession firstCheckOut(opCtx.get());
    expireAllTransactions(opCtx.get());

    auto client =
        getServiceContext()->getService()->makeClient("CheckOutForKillOldestTransactionTimeOut");
    AlternativeClientRegion acr(client);
    auto sideOpCtx = cc().makeOperationContext();
    sideOpCtx->setLogicalSessionId(lsid);

    int64_t numKills = 0;
    int64_t numSkips = 0;
    int64_t numTimeOuts = 0;

    killOldestTransaction(sideOpCtx.get(), Milliseconds(0), &numKills, &numSkips, &numTimeOuts);

    ASSERT_EQ(0, numKills);
    ASSERT_EQ(0, numSkips);
    ASSERT_EQ(1, numTimeOuts);
}

using KillSessionsDeathTest = KillSessionsTest;
DEATH_TEST_F(KillSessionsDeathTest, killSessionsAbortUnpreparedTransactionsTimesOut, "11790802") {
    auto lsid = makeLogicalSessionIdForTest();
    createSession(lsid);
    auto opCtx = makeOperationContext();
    opCtx->setLogicalSessionId(lsid);

    // Check out the session and never release it so it will block the session killer thread.
    OperationContextSession firstCheckOut(opCtx.get());
    auto txnParticipant = TransactionParticipant::get(opCtx.get());
    txnParticipant.transitionToInProgressForTest();

    auto client = getServiceContext()->getService()->makeClient("CheckOutForKillTimeout");
    AlternativeClientRegion acr(client);
    auto killOpCtx = cc().makeOperationContext();
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx.get())});
    ErrorCodes::Error killReason = ErrorCodes::InterruptedDueToReplStateChange;

    // Pass old time as deadline to ensure the function times out.
    Date_t deadline = Date_t::now() - Milliseconds(100);
    killSessionsAbortUnpreparedTransactions(
        killOpCtx.get(), matcherAllSessions, killReason, deadline);
}

TEST_F(KillSessionsTest, killSessionsAbortUnpreparedTransactionsSuccessfully) {
    auto lsid = makeLogicalSessionIdForTest();
    createSession(lsid);
    auto opCtx = makeOperationContext();
    opCtx->setLogicalSessionId(lsid);

    // Check out the session and mark it in progress.
    {
        OperationContextSession firstCheckOut(opCtx.get());
        auto txnParticipant = TransactionParticipant::get(opCtx.get());
        txnParticipant.transitionToInProgressForTest();
        // Timer has to be started in order to update metrics in the transactionMetricsObserver
        // as a part of aborting transactions.
        advanceTransactionMetricsTimer(opCtx.get(), txnParticipant);
    }

    auto client = getServiceContext()->getService()->makeClient("CheckOutForKillTimeout");
    AlternativeClientRegion acr(client);
    auto killOpCtx = cc().makeOperationContext();
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx.get())});
    ErrorCodes::Error killReason = ErrorCodes::InterruptedDueToReplStateChange;

    Date_t deadline = Date_t::now() + Milliseconds(10000);
    killSessionsAbortUnpreparedTransactions(
        killOpCtx.get(), matcherAllSessions, killReason, deadline);
}

// ---------------------------------------------------------------------------
// Tests for killSessionsAbortUnpreparedTransactionsForLockerIds
// ---------------------------------------------------------------------------

class KillSessionsForLockerIdsTest : public KillSessionsTest {
protected:
    // Sets up a session whose TransactionParticipant has a stashed Locker with a known id.
    // Returns (lsid, stashedLockerId). The session is checked back in before returning so the
    // scan in killSessionsAbortUnpreparedTransactionsForLockerIds can observe it.
    std::pair<LogicalSessionId, LockerId> makeSessionWithStashedInProgressTxn(
        bool prepared = false) {
        auto lsid = makeLogicalSessionIdForTest();
        createSession(lsid);
        auto opCtx = makeOperationContext();
        opCtx->setLogicalSessionId(lsid);

        OperationContextSession checkout(opCtx.get());
        auto txnParticipant = TransactionParticipant::get(opCtx.get());
        txnParticipant.transitionToInProgressForTest();
        advanceTransactionMetricsTimer(opCtx.get(), txnParticipant);

        auto stashedId = shard_role_details::getLocker(opCtx.get())->getId();
        txnParticipant.stashActiveTransactionForTest(opCtx.get());

        if (prepared) {
            txnParticipant.transitionToPreparedforTest(opCtx.get(), repl::OpTime({1, 1}, 1));
        }

        return {lsid, stashedId};
    }

    // Runs `fn` while a side client is active. The side opCtx is passed to `fn`.
    template <typename Fn>
    void withSideClient(std::string_view name, Fn&& fn) {
        auto client = getServiceContext()->getService()->makeClient(std::string{name});
        AlternativeClientRegion acr(client);
        auto sideOpCtx = cc().makeOperationContext();
        fn(sideOpCtx.get());
    }

    // Checks out `lsid` on a fresh side opCtx and returns whether the transaction is open.
    bool isTransactionOpen(const LogicalSessionId& lsid) {
        bool open = false;
        withSideClient("verify", [&](OperationContext* sideOpCtx) {
            sideOpCtx->setLogicalSessionId(lsid);
            OperationContextSession verify(sideOpCtx);
            auto txnParticipant = TransactionParticipant::get(sideOpCtx);
            open = txnParticipant.transactionIsOpen();
        });
        return open;
    }
};

TEST_F(KillSessionsForLockerIdsTest, EmptyLockerIdsIsNoOp) {
    auto [lsid, stashedId] = makeSessionWithStashedInProgressTxn();

    withSideClient("killer", [&](OperationContext* killOpCtx) {
        killSessionsAbortUnpreparedTransactionsForLockerIds(
            killOpCtx, /*lockerIds*/ {}, ErrorCodes::Interrupted);
    });

    ASSERT_TRUE(isTransactionOpen(lsid));
}

TEST_F(KillSessionsForLockerIdsTest, NoMatchingLockerIdReturnsWithoutKill) {
    auto [lsid, stashedId] = makeSessionWithStashedInProgressTxn();

    withSideClient("killer", [&](OperationContext* killOpCtx) {
        killSessionsAbortUnpreparedTransactionsForLockerIds(
            killOpCtx, {stashedId + 9999}, ErrorCodes::Interrupted);
    });

    ASSERT_TRUE(isTransactionOpen(lsid));
}

TEST_F(KillSessionsForLockerIdsTest, PreparedTransactionIsNotAborted) {
    auto [lsid, stashedId] = makeSessionWithStashedInProgressTxn(/*prepared=*/true);

    withSideClient("killer", [&](OperationContext* killOpCtx) {
        killSessionsAbortUnpreparedTransactionsForLockerIds(
            killOpCtx, {stashedId}, ErrorCodes::Interrupted);
    });

    // Verify still open (prepared transactions are open).
    ASSERT_TRUE(isTransactionOpen(lsid));
}

TEST_F(KillSessionsForLockerIdsTest, AbortsViaStashedLockerIdMatch) {
    auto [lsid, stashedId] = makeSessionWithStashedInProgressTxn();

    withSideClient("killer", [&](OperationContext* killOpCtx) {
        killSessionsAbortUnpreparedTransactionsForLockerIds(
            killOpCtx, {stashedId}, ErrorCodes::Interrupted);
    });

    ASSERT_FALSE(isTransactionOpen(lsid));
}

// Active session in the background thread is matched and killed.
TEST_F(KillSessionsForLockerIdsTest, AbortsViaActiveLockerIdMatch) {
    auto lsid = makeLogicalSessionIdForTest();
    createSession(lsid);

    std::promise<LockerId> lockerIdPromise;
    auto lockerIdFuture = lockerIdPromise.get_future();

    auto bgFuture = std::async(std::launch::async, [this, lsid, &lockerIdPromise] {
        ThreadClient tc("bg-txn-holder", getServiceContext()->getService());
        auto bgOpCtx = Client::getCurrent()->makeOperationContext();
        bgOpCtx->setLogicalSessionId(lsid);

        OperationContextSession checkout(bgOpCtx.get());
        auto txnParticipant = TransactionParticipant::get(bgOpCtx.get());
        txnParticipant.transitionToInProgressForTest();
        advanceTransactionMetricsTimer(bgOpCtx.get(), txnParticipant);

        auto activeId = shard_role_details::getLocker(bgOpCtx.get())->getId();
        lockerIdPromise.set_value(activeId);

        // Block until the session kill interrupts this opCtx.
        try {
            bgOpCtx->sleepFor(Seconds(30));
        } catch (const ExceptionFor<ErrorCodes::Interrupted>&) {
            // Expected: killSessionsAction called session.kill() which interrupted us.
        }
    });

    auto activeLockerId = lockerIdFuture.get();

    withSideClient("killer", [&](OperationContext* killOpCtx) {
        killSessionsAbortUnpreparedTransactionsForLockerIds(
            killOpCtx, {activeLockerId}, ErrorCodes::Interrupted);
    });

    bgFuture.get();
    ASSERT_FALSE(isTransactionOpen(lsid));
}

// Selectively abort only matching session with stashed resources.
TEST_F(KillSessionsForLockerIdsTest, OnlyMatchingSessionIsAbortedAmongMany) {
    auto [lsidA, idA] = makeSessionWithStashedInProgressTxn();
    auto [lsidB, idB] = makeSessionWithStashedInProgressTxn();
    auto [lsidC, idC] = makeSessionWithStashedInProgressTxn();

    withSideClient("killer", [&](OperationContext* killOpCtx) {
        killSessionsAbortUnpreparedTransactionsForLockerIds(
            killOpCtx, {idB}, ErrorCodes::Interrupted);
    });

    ASSERT_TRUE(isTransactionOpen(lsidA));
    ASSERT_FALSE(isTransactionOpen(lsidB));
    ASSERT_TRUE(isTransactionOpen(lsidC));
}

}  // namespace
}  // namespace mongo

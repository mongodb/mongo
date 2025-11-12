/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/session/kill_sessions_local.h"

#include "mongo/db/session/session_catalog_test.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

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
            // Timer as to be started in order to update metrics in the transactionMetricsObserver
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

}  // namespace
}  // namespace mongo

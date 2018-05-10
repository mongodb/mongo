/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class SessionCatalogTest : public MockReplCoordServerFixture {
protected:
    void setUp() final {
        MockReplCoordServerFixture::setUp();

        auto service = opCtx()->getServiceContext();
        SessionCatalog::get(service)->reset_forTest();
        SessionCatalog::get(service)->onStepUp(opCtx());
    }

    SessionCatalog* catalog() {
        return SessionCatalog::get(opCtx()->getServiceContext());
    }
};

// When this class is in scope, makes the system behave as if we're in a DBDirectClient
class DirectClientSetter {
public:
    explicit DirectClientSetter(OperationContext* opCtx)
        : _opCtx(opCtx), _wasInDirectClient(opCtx->getClient()->isInDirectClient()) {
        opCtx->getClient()->setInDirectClient(true);
    }

    ~DirectClientSetter() {
        _opCtx->getClient()->setInDirectClient(_wasInDirectClient);
    }

private:
    const OperationContext* _opCtx;
    const bool _wasInDirectClient;
};

TEST_F(SessionCatalogTest, CheckoutAndReleaseSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());

    auto scopedSession = catalog()->checkOutSession(opCtx());

    ASSERT(scopedSession.get());
    ASSERT_EQ(*opCtx()->getLogicalSessionId(), scopedSession->getSessionId());
}

TEST_F(SessionCatalogTest, OperationContextCheckedOutSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    const TxnNumber txnNum = 20;
    opCtx()->setTxnNumber(txnNum);

    OperationContextSession ocs(opCtx(), true, boost::none, boost::none, "testDB", "insert");
    auto session = OperationContextSession::get(opCtx());
    ASSERT(session);
    ASSERT_EQ(*opCtx()->getLogicalSessionId(), session->getSessionId());
}

TEST_F(SessionCatalogTest, OperationContextNonCheckedOutSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());

    OperationContextSession ocs(opCtx(), false, boost::none, boost::none, "testDB", "insert");
    auto session = OperationContextSession::get(opCtx());

    ASSERT(!session);
}

TEST_F(SessionCatalogTest, GetOrCreateNonExistentSession) {
    const auto lsid = makeLogicalSessionIdForTest();
    auto scopedSession = catalog()->getOrCreateSession(opCtx(), lsid);

    ASSERT(scopedSession.get());
    ASSERT_EQ(lsid, scopedSession->getSessionId());
}

TEST_F(SessionCatalogTest, GetOrCreateSessionAfterCheckOutSession) {
    const auto lsid = makeLogicalSessionIdForTest();
    opCtx()->setLogicalSessionId(lsid);

    boost::optional<OperationContextSession> ocs;
    ocs.emplace(opCtx(), true, boost::none, false, "testDB", "insert");

    stdx::async(stdx::launch::async, [&] {
        Client::initThreadIfNotAlready();
        auto sideOpCtx = Client::getCurrent()->makeOperationContext();
        auto scopedSession =
            SessionCatalog::get(sideOpCtx.get())->getOrCreateSession(sideOpCtx.get(), lsid);

        ASSERT(scopedSession.get());
        ASSERT_EQ(lsid, scopedSession->getSessionId());
    }).get();

    ocs.reset();

    stdx::async(stdx::launch::async, [&] {
        Client::initThreadIfNotAlready();
        auto sideOpCtx = Client::getCurrent()->makeOperationContext();
        auto scopedSession =
            SessionCatalog::get(sideOpCtx.get())->getOrCreateSession(sideOpCtx.get(), lsid);

        ASSERT(scopedSession.get());
        ASSERT_EQ(lsid, scopedSession->getSessionId());
    }).get();
}

TEST_F(SessionCatalogTest, NestedOperationContextSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        OperationContextSession outerScopedSession(
            opCtx(), true, boost::none, boost::none, "testDB", "insert");

        {
            DirectClientSetter inDirectClient(opCtx());
            OperationContextSession innerScopedSession(
                opCtx(), true, boost::none, boost::none, "testDB", "insert");

            auto session = OperationContextSession::get(opCtx());
            ASSERT(session);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), session->getSessionId());
        }

        {
            DirectClientSetter inDirectClient(opCtx());
            auto session = OperationContextSession::get(opCtx());
            ASSERT(session);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), session->getSessionId());
        }
    }

    ASSERT(!OperationContextSession::get(opCtx()));
}

TEST_F(SessionCatalogTest, StashInNestedSessionIsANoop) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(1);

    {
        OperationContextSession outerScopedSession(
            opCtx(), true, boost::none, boost::none, "testDB", "find");

        Locker* originalLocker = opCtx()->lockState();
        RecoveryUnit* originalRecoveryUnit = opCtx()->recoveryUnit();
        ASSERT(originalLocker);
        ASSERT(originalRecoveryUnit);

        // Set the readConcern on the OperationContext.
        repl::ReadConcernArgs readConcernArgs;
        ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                                  << "test"
                                                  << repl::ReadConcernArgs::kReadConcernFieldName
                                                  << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                          << "snapshot"))));
        repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

        // Perform initial unstash, which sets up a WriteUnitOfWork.
        OperationContextSession::get(opCtx())->unstashTransactionResources(opCtx(), "find");
        ASSERT_EQUALS(originalLocker, opCtx()->lockState());
        ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
        ASSERT(opCtx()->getWriteUnitOfWork());

        {
            // Make it look like we're in a DBDirectClient running a nested operation.
            DirectClientSetter inDirectClient(opCtx());
            OperationContextSession innerScopedSession(
                opCtx(), true, boost::none, boost::none, "testDB", "find");

            // Report to Session that there is a stashed cursor. If we were not in a nested session,
            // this would ensure that stashing is not a noop.
            Session::registerCursorExistsFunction([](LogicalSessionId, TxnNumber) { return true; });

            OperationContextSession::get(opCtx())->stashTransactionResources(opCtx());

            // The stash was a noop, so the locker, RecoveryUnit, and WriteUnitOfWork on the
            // OperationContext are unaffected.
            ASSERT_EQUALS(originalLocker, opCtx()->lockState());
            ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
            ASSERT(opCtx()->getWriteUnitOfWork());
        }
    }
}

TEST_F(SessionCatalogTest, UnstashInNestedSessionIsANoop) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(1);

    {
        OperationContextSession outerScopedSession(
            opCtx(), true, boost::none, boost::none, "testDB", "find");

        Locker* originalLocker = opCtx()->lockState();
        RecoveryUnit* originalRecoveryUnit = opCtx()->recoveryUnit();
        ASSERT(originalLocker);
        ASSERT(originalRecoveryUnit);

        // Set the readConcern on the OperationContext.
        repl::ReadConcernArgs readConcernArgs;
        ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                                  << "test"
                                                  << repl::ReadConcernArgs::kReadConcernFieldName
                                                  << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                          << "snapshot"))));
        repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

        {
            // Make it look like we're in a DBDirectClient running a nested operation.
            DirectClientSetter inDirectClient(opCtx());
            OperationContextSession innerScopedSession(
                opCtx(), true, boost::none, boost::none, "testDB", "find");

            OperationContextSession::get(opCtx())->unstashTransactionResources(opCtx(), "find");

            // The unstash was a noop, so the OperationContext did not get a WriteUnitOfWork.
            ASSERT_EQUALS(originalLocker, opCtx()->lockState());
            ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
            ASSERT_FALSE(opCtx()->getWriteUnitOfWork());
        }
    }
}

TEST_F(SessionCatalogTest, ScanSessions) {
    std::vector<LogicalSessionId> lsids;
    auto workerFn = [&](OperationContext* opCtx, Session* session) {
        lsids.push_back(session->getSessionId());
    };

    // Scan over zero Sessions.
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx())});
    catalog()->scanSessions(opCtx(), matcherAllSessions, workerFn);
    ASSERT(lsids.empty());

    // Create three sessions in the catalog.
    auto lsid1 = makeLogicalSessionIdForTest();
    auto lsid2 = makeLogicalSessionIdForTest();
    auto lsid3 = makeLogicalSessionIdForTest();
    {
        auto scopedSession1 = catalog()->getOrCreateSession(opCtx(), lsid1);
        auto scopedSession2 = catalog()->getOrCreateSession(opCtx(), lsid2);
        auto scopedSession3 = catalog()->getOrCreateSession(opCtx(), lsid3);
    }

    // Scan over all Sessions.
    lsids.clear();
    catalog()->scanSessions(opCtx(), matcherAllSessions, workerFn);
    ASSERT_EQ(lsids.size(), 3U);

    // Scan over all Sessions, visiting a particular Session.
    SessionKiller::Matcher matcherLSID2(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx(), lsid2)});
    lsids.clear();
    catalog()->scanSessions(opCtx(), matcherLSID2, workerFn);
    ASSERT_EQ(lsids.size(), 1U);
    ASSERT_EQ(lsids.front(), lsid2);
}

}  // namespace
}  // namespace mongo

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
        SessionCatalog::reset_forTest(service);
        SessionCatalog::create(service);
        SessionCatalog::get(service)->onStepUp(opCtx());
    }

    SessionCatalog* catalog() {
        return SessionCatalog::get(opCtx()->getServiceContext());
    }
};

TEST_F(SessionCatalogTest, CheckoutAndReleaseSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());

    auto scopedSession = catalog()->checkOutSession(opCtx());

    ASSERT(scopedSession.get());
    ASSERT_EQ(*opCtx()->getLogicalSessionId(), scopedSession->getSessionId());
}

TEST_F(SessionCatalogTest, OperationContextSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        OperationContextSession ocs(opCtx(), true, boost::none);
        auto session = OperationContextSession::get(opCtx());

        ASSERT(session);
        ASSERT_EQ(*opCtx()->getLogicalSessionId(), session->getSessionId());

        // Confirm that stash and unstash can be executed against a top-level checked-out Session.
        ocs.stashTransactionResources();
        ocs.unstashTransactionResources();
    }

    {
        OperationContextSession ocs(opCtx(), false, boost::none);
        auto session = OperationContextSession::get(opCtx());

        ASSERT(!session);

        // Confirm that stash and unstash can be executed against a top-level not-checked-out
        // Session.
        ocs.stashTransactionResources();
        ocs.unstashTransactionResources();
    }
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
    ocs.emplace(opCtx(), true, boost::none);

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
        OperationContextSession outerScopedSession(opCtx(), true, boost::none);

        {
            OperationContextSession innerScopedSession(opCtx(), true, boost::none);

            auto session = OperationContextSession::get(opCtx());
            ASSERT(session);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), session->getSessionId());
        }

        {
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
        OperationContextSession outerScopedSession(opCtx(), true, boost::none);

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
        outerScopedSession.unstashTransactionResources();
        ASSERT_EQUALS(originalLocker, opCtx()->lockState());
        ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
        ASSERT(opCtx()->getWriteUnitOfWork());

        {
            OperationContextSession innerScopedSession(opCtx(), true, boost::none);

            // Indicate that there is a stashed cursor. If we were not in a nested session, this
            // would ensure that stashing is not a noop.
            opCtx()->setStashedCursor();

            innerScopedSession.stashTransactionResources();

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
        OperationContextSession outerScopedSession(opCtx(), true, boost::none);

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
            OperationContextSession innerScopedSession(opCtx(), true, boost::none);

            innerScopedSession.unstashTransactionResources();

            // The unstash was a noop, so the OperationContext did not get a WriteUnitOfWork.
            ASSERT_EQUALS(originalLocker, opCtx()->lockState());
            ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
            ASSERT_FALSE(opCtx()->getWriteUnitOfWork());
        }
    }
}

TEST_F(SessionCatalogTest, OnlyCheckOutSessionWithCheckOutSessionTrue) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        OperationContextSession ocs(opCtx(), true, boost::none);
        ASSERT(OperationContextSession::get(opCtx()));
    }

    {
        OperationContextSession ocs(opCtx(), false, boost::none);
        ASSERT(!OperationContextSession::get(opCtx()));
    }
}

}  // namespace
}  // namespace mongo


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

#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

class SessionCatalogTest : public ServiceContextMongoDTest {
protected:
    void setUp() final {
        ServiceContextMongoDTest::setUp();

        catalog()->reset_forTest();
    }

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

    OperationContextSession ocs(_opCtx, true);
    auto session = OperationContextSession::get(_opCtx);
    ASSERT(session);
    ASSERT_EQ(*_opCtx->getLogicalSessionId(), session->getSessionId());
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, OperationContextNonCheckedOutSession) {
    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());

    OperationContextSession ocs(_opCtx, false);
    auto session = OperationContextSession::get(_opCtx);

    ASSERT(!session);
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, GetOrCreateNonExistentSession) {
    const auto lsid = makeLogicalSessionIdForTest();
    auto scopedSession = catalog()->getOrCreateSession(_opCtx, lsid);

    ASSERT(scopedSession.get());
    ASSERT_EQ(lsid, scopedSession->getSessionId());
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, GetOrCreateSessionAfterCheckOutSession) {
    const auto lsid = makeLogicalSessionIdForTest();
    _opCtx->setLogicalSessionId(lsid);

    boost::optional<OperationContextSession> ocs;
    ocs.emplace(_opCtx, true);

    stdx::async(stdx::launch::async, [&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready();
        auto sideOpCtx = Client::getCurrent()->makeOperationContext();
        auto scopedSession =
            SessionCatalog::get(sideOpCtx.get())->getOrCreateSession(sideOpCtx.get(), lsid);

        ASSERT(scopedSession.get());
        ASSERT_EQ(lsid, scopedSession->getSessionId());
    }).get();

    ocs.reset();

    stdx::async(stdx::launch::async, [&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready();
        auto sideOpCtx = Client::getCurrent()->makeOperationContext();
        auto scopedSession =
            SessionCatalog::get(sideOpCtx.get())->getOrCreateSession(sideOpCtx.get(), lsid);

        ASSERT(scopedSession.get());
        ASSERT_EQ(lsid, scopedSession->getSessionId());
    }).get();
}

TEST_F(SessionCatalogTestWithDefaultOpCtx, NestedOperationContextSession) {
    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        OperationContextSession outerScopedSession(_opCtx, true);

        {
            DirectClientSetter inDirectClient(_opCtx);
            OperationContextSession innerScopedSession(_opCtx, true);

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
    std::vector<LogicalSessionId> lsids;
    auto workerFn = [&](Session* session) { lsids.push_back(session->getSessionId()); };

    // Scan over zero Sessions.
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx)});
    catalog()->scanSessions(matcherAllSessions, workerFn);
    ASSERT(lsids.empty());

    // Create three sessions in the catalog.
    auto lsid1 = makeLogicalSessionIdForTest();
    auto lsid2 = makeLogicalSessionIdForTest();
    auto lsid3 = makeLogicalSessionIdForTest();
    {
        auto scopedSession1 = catalog()->getOrCreateSession(_opCtx, lsid1);
        auto scopedSession2 = catalog()->getOrCreateSession(_opCtx, lsid2);
        auto scopedSession3 = catalog()->getOrCreateSession(_opCtx, lsid3);
    }

    // Scan over all Sessions.
    lsids.clear();
    catalog()->scanSessions(matcherAllSessions, workerFn);
    ASSERT_EQ(lsids.size(), 3U);

    // Scan over all Sessions, visiting a particular Session.
    SessionKiller::Matcher matcherLSID2(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(_opCtx, lsid2)});
    lsids.clear();
    catalog()->scanSessions(matcherLSID2, workerFn);
    ASSERT_EQ(lsids.size(), 1U);
    ASSERT_EQ(lsids.front(), lsid2);
}

}  // namespace
}  // namespace mongo

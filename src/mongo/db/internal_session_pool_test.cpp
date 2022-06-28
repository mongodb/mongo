/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/internal_session_pool.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_cache_impl.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/service_liaison_mock.h"
#include "mongo/db/sessions_collection_mock.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class InternalSessionPoolTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        _pool = InternalSessionPool::get(getServiceContext());
        _opCtx = makeOperationContext();
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

protected:
    InternalSessionPool* _pool;

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(InternalSessionPoolTest, AcquireStandaloneSessionFromEmptyPool) {
    auto session = _pool->acquireStandaloneSession(opCtx());
    ASSERT_EQ(TxnNumber(0), session.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, AcquireWithParentSessionFromEmptyPool) {
    auto parentLsid = makeLogicalSessionIdForTest();
    auto session = _pool->acquireChildSession(opCtx(), parentLsid);

    ASSERT_EQ(parentLsid, *getParentSessionId(session.getSessionId()));
    ASSERT_EQ(TxnNumber(0), session.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, AcquireStandaloneSessionFromPool) {
    auto expectedLsid = makeLogicalSessionIdForTest();
    auto sessionToRelease = InternalSessionPool::Session(expectedLsid, TxnNumber(0));
    _pool->release(sessionToRelease);

    auto session = _pool->acquireStandaloneSession(opCtx());

    ASSERT_EQ(expectedLsid, session.getSessionId());
    // txnNumber should be 1 larger than the released session.
    ASSERT_EQ(TxnNumber(1), session.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, AcquireWithParentSessionFromPoolWithoutParentEntry) {
    LogicalSessionId parentLsid1 = makeLogicalSessionIdForTest();
    LogicalSessionId parentLsid2 = makeLogicalSessionIdForTest();

    auto parentSession1 = InternalSessionPool::Session(parentLsid1, TxnNumber(1));
    _pool->release(parentSession1);

    auto session = _pool->acquireChildSession(opCtx(), parentLsid2);

    ASSERT_NOT_EQUALS(parentLsid1, session.getSessionId());
    ASSERT_EQ(TxnNumber(0), session.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, AcquireChildForSameParentWithoutIntermediateRelease) {
    LogicalSessionId parentLsid = makeLogicalSessionIdForTest();

    auto childLsid1 = makeLogicalSessionIdWithTxnUUID(parentLsid);
    _pool->release(InternalSessionPool::Session(childLsid1, TxnNumber(0)));

    auto childSession1 = _pool->acquireChildSession(opCtx(), parentLsid);

    ASSERT_NOT_EQUALS(parentLsid, childSession1.getSessionId());
    ASSERT_EQ(childLsid1, childSession1.getSessionId());
    ASSERT_EQ(TxnNumber(1), childSession1.getTxnNumber());

    // If we acquire a child for the same parent without first releasing the checked out child, it
    // shouldn't block or prevent future child session reuse.

    auto childSession2 = _pool->acquireChildSession(opCtx(), parentLsid);
    ASSERT_NOT_EQUALS(parentLsid, childSession2.getSessionId());
    ASSERT_NOT_EQUALS(childLsid1, childSession2.getSessionId());
    ASSERT_EQ(TxnNumber(0), childSession2.getTxnNumber());

    _pool->release(childSession2);

    auto childSession3 = _pool->acquireChildSession(opCtx(), parentLsid);
    ASSERT_EQ(childSession2.getSessionId(), childSession3.getSessionId());
    ASSERT_EQ(TxnNumber(1), childSession3.getTxnNumber());

    // Releasing the first child session back into the pool should overwrite the previous session
    // and still allow for reuse.

    _pool->release(childSession1);

    auto childSession4 = _pool->acquireChildSession(opCtx(), parentLsid);
    ASSERT_EQ(childSession1.getSessionId(), childSession4.getSessionId());
    ASSERT_EQ(TxnNumber(2), childSession4.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, AcquireWithParentSessionFromPoolWithParentEntry) {
    LogicalSessionId parentLsid1 = makeLogicalSessionIdForTest();
    LogicalSessionId parentLsid2 = makeLogicalSessionIdForTest();

    // Create child sessions for each parent session and release them in the pool.
    auto childLsid1 = makeLogicalSessionIdWithTxnUUID(parentLsid1);
    auto childLsid2 = makeLogicalSessionIdWithTxnUUID(parentLsid2);

    _pool->release(InternalSessionPool::Session(childLsid1, TxnNumber(1)));
    _pool->release(InternalSessionPool::Session(childLsid2, TxnNumber(2)));

    auto childSession2 = _pool->acquireChildSession(opCtx(), parentLsid2);

    ASSERT_NOT_EQUALS(parentLsid1, childSession2.getSessionId());
    ASSERT_NOT_EQUALS(childLsid1, childSession2.getSessionId());
    ASSERT_NOT_EQUALS(parentLsid2, childSession2.getSessionId());
    ASSERT_EQ(childLsid2, childSession2.getSessionId());

    // txnNumber should be 1 larger than the released parent session.
    ASSERT_EQ(TxnNumber(3), childSession2.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, ReuseUnexpiredStandaloneSessionForSystemUser) {
    auto expectedLsid = makeSystemLogicalSessionId();
    auto sessionToRelease = InternalSessionPool::Session(expectedLsid, TxnNumber(0));
    _pool->release(sessionToRelease);

    auto session = _pool->acquireSystemSession();
    ASSERT_EQ(expectedLsid, session.getSessionId());

    // txnNumber should be 1 larger than the released session.
    ASSERT_EQ(TxnNumber(1), session.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, ReuseUnexpiredStandaloneSession) {
    auto session = _pool->acquireStandaloneSession(opCtx());
    auto expectedLsid = session.getSessionId();
    _pool->release(session);

    auto acquiredSession = _pool->acquireStandaloneSession(opCtx());
    ASSERT_EQ(expectedLsid, session.getSessionId());

    // txnNumber should be 1 larger than the released session.
    ASSERT_EQ(TxnNumber(1), acquiredSession.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, StandaloneSessionAcquiredInReverseOrder) {
    auto expectedLsid1 = makeLogicalSessionId(opCtx());
    auto expectedLsid2 = makeLogicalSessionId(opCtx());
    auto sessionToRelease1 = InternalSessionPool::Session(expectedLsid1, TxnNumber(0));
    auto sessionToRelease2 = InternalSessionPool::Session(expectedLsid2, TxnNumber(0));
    _pool->release(sessionToRelease1);
    _pool->release(sessionToRelease2);

    auto session = _pool->acquireStandaloneSession(opCtx());
    ASSERT_EQ(expectedLsid2, session.getSessionId());
    // txnNumber should be 1 larger than the released session.
    ASSERT_EQ(TxnNumber(1), session.getTxnNumber());

    auto nextSession = _pool->acquireStandaloneSession(opCtx());
    ASSERT_EQ(expectedLsid1, nextSession.getSessionId());
    // txnNumber should be 1 larger than the released session.
    ASSERT_EQ(TxnNumber(1), nextSession.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, UserCannotAcquireSystemStandaloneSession) {
    auto lsid = makeLogicalSessionId(opCtx());
    auto sessionToRelease = InternalSessionPool::Session(lsid, TxnNumber(0));
    _pool->release(sessionToRelease);

    auto systemLsid = makeSystemLogicalSessionId();
    _pool->release(InternalSessionPool::Session(systemLsid, TxnNumber(0)));

    auto session = _pool->acquireStandaloneSession(opCtx());
    ASSERT_NE(systemLsid, session.getSessionId());
    ASSERT_EQ(lsid, session.getSessionId());
    // txnNumber should be 1 larger than the released session.
    ASSERT_EQ(TxnNumber(1), session.getTxnNumber());
    _pool->release(session);

    auto nextSession = _pool->acquireSystemSession();
    ASSERT_NE(lsid, nextSession.getSessionId());
    ASSERT_EQ(systemLsid, nextSession.getSessionId());
    // txnNumber should be 1 larger than the released session.
    ASSERT_EQ(TxnNumber(1), nextSession.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, StaleStandaloneSessionIsNotAcquired) {
    auto service = getServiceContext();
    const std::shared_ptr<ClockSourceMock> mockClock = std::make_shared<ClockSourceMock>();
    service->setFastClockSource(std::make_unique<SharedClockSourceAdapter>(mockClock));

    auto expectedLsid = makeSystemLogicalSessionId();
    auto sessionToRelease = InternalSessionPool::Session(expectedLsid, TxnNumber(0));
    _pool->release(sessionToRelease);
    mockClock->advance(Minutes{localLogicalSessionTimeoutMinutes});

    auto acquiredSession = _pool->acquireSystemSession();
    ASSERT_NE(expectedLsid, acquiredSession.getSessionId());
    // Should be a new session.
    ASSERT_EQ(TxnNumber(0), acquiredSession.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, ExpiredSessionReapedDuringRelease) {
    auto service = getServiceContext();
    const std::shared_ptr<ClockSourceMock> mockClock = std::make_shared<ClockSourceMock>();
    service->setFastClockSource(std::make_unique<SharedClockSourceAdapter>(mockClock));

    auto lsid0 = makeSystemLogicalSessionId();
    auto session0 = InternalSessionPool::Session(lsid0, TxnNumber(0));
    _pool->release(session0);

    auto lsid1 = makeSystemLogicalSessionId();
    auto session1 = InternalSessionPool::Session(lsid1, TxnNumber(0));
    mockClock->advance(Minutes{localLogicalSessionTimeoutMinutes});
    _pool->release(session1);

    // Both system sessions have the same userDigest, but the session with lsid0 should've been
    // removed during release.
    ASSERT_EQ(1, _pool->numSessionsForUser_forTest(lsid1.getUid()));
}

TEST_F(InternalSessionPoolTest, UserExpiredSessionsReapedWhenAnotherUserReleasesSession) {
    auto service = getServiceContext();
    const std::shared_ptr<ClockSourceMock> mockClock = std::make_shared<ClockSourceMock>();
    service->setFastClockSource(std::make_unique<SharedClockSourceAdapter>(mockClock));

    auto lsid0 = makeLogicalSessionId(opCtx());
    auto sessionToRelease = InternalSessionPool::Session(lsid0, TxnNumber(0));
    _pool->release(sessionToRelease);

    auto lsid1 = makeSystemLogicalSessionId();
    auto session1 = InternalSessionPool::Session(lsid1, TxnNumber(0));
    mockClock->advance(Minutes{localLogicalSessionTimeoutMinutes});
    _pool->release(session1);

    // The session with lsid0 should've been removed from the session pool when lsid1 was released.
    ASSERT_EQ(0, _pool->numSessionsForUser_forTest(lsid0.getUid()));
    ASSERT_EQ(1, _pool->numSessionsForUser_forTest(lsid1.getUid()));
}

}  // namespace
}  // namespace mongo

/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/primary_only_service_helpers/operation_session_tracker.h"

#include "mongo/db/service_context_test_fixture.h"

namespace mongo {
namespace {
OperationSessionInfo makeOperationSessionInfoForTest() {
    OperationSessionInfo osi;
    osi.setSessionId(makeLogicalSessionIdForTest());
    osi.setTxnNumber(TxnNumber(1));
    return osi;
}

class OperationSessionPersistenceForTest : public OperationSessionPersistence {
public:
    boost::optional<OperationSessionInfo> readSession(OperationContext* opCtx) const override {
        return _session;
    }
    void writeSession(OperationContext* opCtx,
                      const boost::optional<OperationSessionInfo>& osi) override {
        _session = osi;
    }

private:
    boost::optional<OperationSessionInfo> _session;
};

class CausalityBarrierForTest : public CausalityBarrier {
public:
    void perform(OperationContext* opCtx, const OperationSessionInfo& osi) override {
        _lastSeen = osi;
    }

    boost::optional<OperationSessionInfo> getLastSeen() const {
        return _lastSeen;
    }

private:
    boost::optional<OperationSessionInfo> _lastSeen;
};

class OperationSessionTrackerTest : public ServiceContextTest {
public:
    OperationSessionTrackerTest() : _tracker(&_persistence) {}

    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
    }

    OperationSessionPersistenceForTest& getPersistence() {
        return _persistence;
    }

    CausalityBarrierForTest& getBarrier() {
        return _barrier;
    }

    OperationSessionTracker& getTracker() {
        return _tracker;
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

private:
    OperationSessionPersistenceForTest _persistence;
    CausalityBarrierForTest _barrier;
    OperationSessionTracker _tracker;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(OperationSessionTrackerTest, GetCurrentSessionNoSession) {
    ASSERT_EQ(getTracker().getCurrentSession(opCtx()), boost::none);
}

TEST_F(OperationSessionTrackerTest, GetNextSession) {
    auto firstSession = getTracker().getNextSession(opCtx());
    ASSERT_NE(firstSession.getSessionId(), boost::none);
    ASSERT_NE(firstSession.getTxnNumber(), boost::none);
    ASSERT_BSONOBJ_EQ(firstSession.toBSON(), getTracker().getCurrentSession(opCtx())->toBSON());

    auto nextSession = getTracker().getNextSession(opCtx());
    ASSERT_EQ(firstSession.getSessionId(), nextSession.getSessionId());
    ASSERT_EQ(*firstSession.getTxnNumber() + 1, *nextSession.getTxnNumber());
    ASSERT_BSONOBJ_EQ(nextSession.toBSON(), getTracker().getCurrentSession(opCtx())->toBSON());
}

TEST_F(OperationSessionTrackerTest, CurrentSessionReadsFromPersistence) {
    auto osi = makeOperationSessionInfoForTest();
    getPersistence().writeSession(opCtx(), osi);
    ASSERT_BSONOBJ_EQ(osi.toBSON(), getTracker().getCurrentSession(opCtx())->toBSON());
}

TEST_F(OperationSessionTrackerTest, NextSessionUpdatesPersistence) {
    auto session = getTracker().getNextSession(opCtx());
    auto persisted = getPersistence().readSession(opCtx());
    ASSERT_NE(persisted, boost::none);
    ASSERT_BSONOBJ_EQ(session.toBSON(), persisted->toBSON());
}

TEST_F(OperationSessionTrackerTest, GetNextSessionResumesFromPersistedSession) {
    auto osi = makeOperationSessionInfoForTest();
    getPersistence().writeSession(opCtx(), osi);

    auto nextSession = getTracker().getNextSession(opCtx());
    ASSERT_EQ(osi.getSessionId(), nextSession.getSessionId());
    ASSERT_EQ(*osi.getTxnNumber() + 1, *nextSession.getTxnNumber());
}

TEST_F(OperationSessionTrackerTest, ReleaseSessionNoOpWhenNoSession) {
    getTracker().releaseSession(opCtx());
    ASSERT_EQ(getTracker().getCurrentSession(opCtx()), boost::none);
}

TEST_F(OperationSessionTrackerTest, ReleaseSessionClearsPersistence) {
    getTracker().getNextSession(opCtx());
    ASSERT_NE(getTracker().getCurrentSession(opCtx()), boost::none);

    getTracker().releaseSession(opCtx());
    ASSERT_EQ(getTracker().getCurrentSession(opCtx()), boost::none);
}

TEST_F(OperationSessionTrackerTest, ReleaseSessionReturnsSessionToPool) {
    auto session = getTracker().getNextSession(opCtx());
    getTracker().releaseSession(opCtx());

    auto reacquired = getTracker().getNextSession(opCtx());
    ASSERT_EQ(session.getSessionId(), reacquired.getSessionId());
    ASSERT_EQ(*session.getTxnNumber() + 1, *reacquired.getTxnNumber());
}

TEST_F(OperationSessionTrackerTest, PerformCausalityBarrierAcquiresSession) {
    ASSERT_EQ(getBarrier().getLastSeen(), boost::none);

    getTracker().performCausalityBarrier(opCtx(), getBarrier());

    auto barrierSession = getBarrier().getLastSeen();
    ASSERT_NE(barrierSession, boost::none);
    ASSERT_NE(barrierSession->getSessionId(), boost::none);
    ASSERT_NE(barrierSession->getTxnNumber(), boost::none);
    ASSERT_BSONOBJ_EQ(barrierSession->toBSON(), getTracker().getCurrentSession(opCtx())->toBSON());
}

TEST_F(OperationSessionTrackerTest, PerformCausalityBarrierIncrementsSession) {
    auto firstSession = getTracker().getNextSession(opCtx());
    getTracker().performCausalityBarrier(opCtx(), getBarrier());

    auto barrierSession = getBarrier().getLastSeen();
    ASSERT_NE(barrierSession, boost::none);
    ASSERT_EQ(firstSession.getSessionId(), barrierSession->getSessionId());
    ASSERT_EQ(*firstSession.getTxnNumber() + 1, *barrierSession->getTxnNumber());
}

}  // namespace
}  // namespace mongo

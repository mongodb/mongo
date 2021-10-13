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

namespace mongo {
namespace {

class TestInternalSessionPool : public InternalSessionPool {
public:
    void reset() {
        _childSessions = LogicalSessionIdMap<InternalSessionPool::Session>();
        _nonChildSessions = std::stack<InternalSessionPool::Session>();
    }
};
class InternalSessionPoolTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        _pool.reset();

        _opCtx = makeOperationContext();

        auto localServiceLiaison =
            std::make_unique<MockServiceLiaison>(std::make_shared<MockServiceLiaisonImpl>());
        auto localSessionsCollection = std::make_unique<MockSessionsCollection>(
            std::make_shared<MockSessionsCollectionImpl>());

        auto localLogicalSessionCache = std::make_unique<LogicalSessionCacheImpl>(
            std::move(localServiceLiaison),
            std::move(localSessionsCollection),
            [](OperationContext*, SessionsCollection&, Date_t) {
                return 0; /* No op*/
            });

        LogicalSessionCache::set(getServiceContext(), std::move(localLogicalSessionCache));
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

protected:
    TestInternalSessionPool _pool;
    RAIIServerParameterControllerForTest _featureFlagInternalTransactions{
        "featureFlagInternalTransactions", true};

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(InternalSessionPoolTest, AcquireWithoutParentSessionFromEmptyPool) {
    auto session = _pool.acquire(opCtx());
    ASSERT_EQ(TxnNumber(0), session.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, AcquireWithParentSessionFromEmptyPool) {
    auto parentLsid = makeLogicalSessionIdForTest();
    auto session = _pool.acquire(opCtx(), parentLsid);

    ASSERT_EQ(parentLsid, *getParentSessionId(session.getSessionId()));
    ASSERT_EQ(TxnNumber(0), session.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, AcquireWithoutParentSessionFromPool) {
    auto expectedLsid = makeLogicalSessionIdForTest();
    auto sessionToRelease = InternalSessionPool::Session(expectedLsid, TxnNumber(0));
    _pool.release(sessionToRelease);

    auto session = _pool.acquire(opCtx());

    ASSERT_EQ(expectedLsid, session.getSessionId());

    // txnNumber should be 1 larger than the released session.
    ASSERT_EQ(TxnNumber(1), session.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, AcquireWithParentSessionFromPoolWithoutParentEntry) {
    LogicalSessionId parentLsid1 = makeLogicalSessionIdForTest();
    LogicalSessionId parentLsid2 = makeLogicalSessionIdForTest();

    auto parentSession1 = InternalSessionPool::Session(parentLsid1, TxnNumber(1));
    _pool.release(parentSession1);

    auto session = _pool.acquire(opCtx(), parentLsid2);

    ASSERT_NOT_EQUALS(parentLsid1, session.getSessionId());
    ASSERT_EQ(TxnNumber(0), session.getTxnNumber());
}

TEST_F(InternalSessionPoolTest, AcquireWithParentSessionFromPoolWithParentEntry) {
    LogicalSessionId parentLsid1 = makeLogicalSessionIdForTest();
    LogicalSessionId parentLsid2 = makeLogicalSessionIdForTest();

    // Set txnUUID for parentLsids.
    parentLsid1.getInternalSessionFields().setTxnUUID(UUID::gen());
    parentLsid2.getInternalSessionFields().setTxnUUID(UUID::gen());

    auto parentSession1 = InternalSessionPool::Session(parentLsid1, TxnNumber(1));
    _pool.release(parentSession1);

    auto parentSession2 = InternalSessionPool::Session(parentLsid2, TxnNumber(2));
    _pool.release(parentSession2);

    auto childSession2 = _pool.acquire(opCtx(), parentLsid2);

    ASSERT_NOT_EQUALS(parentLsid1, childSession2.getSessionId());
    ASSERT_EQ(parentLsid2, childSession2.getSessionId());

    // txnNumber should be 1 larger than the released parent session.
    ASSERT_EQ(TxnNumber(3), childSession2.getTxnNumber());
}

}  // namespace
}  // namespace mongo

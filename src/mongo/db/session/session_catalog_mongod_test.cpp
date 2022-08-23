/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/session/sessions_collection_mock.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class MongoDSessionCatalogTest : public ServiceContextMongoDTest {
protected:
    MongoDSessionCatalogTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {}

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        const auto service = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        repl::ReplicationCoordinator::set(service, std::move(replCoord));
        repl::createOplog(_opCtx);
    }

    ClockSourceMock* clock() {
        return dynamic_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    ServiceContext::UniqueOperationContext _uniqueOpCtx{makeOperationContext()};
    OperationContext* _opCtx{_uniqueOpCtx.get()};

    std::shared_ptr<MockSessionsCollectionImpl> _collectionMock{
        std::make_shared<MockSessionsCollectionImpl>()};

    std::shared_ptr<SessionsCollection> _collection{
        std::make_shared<MockSessionsCollection>(_collectionMock)};
};

TEST_F(MongoDSessionCatalogTest, ReapSomeExpiredSomeNot) {
    // Create some "old" sessions
    DBDirectClient client(_opCtx);
    SessionTxnRecord txn1(
        makeLogicalSessionIdForTest(), 100, repl::OpTime(Timestamp(100), 1), clock()->now());
    SessionTxnRecord txn2(
        makeLogicalSessionIdForTest(), 200, repl::OpTime(Timestamp(200), 1), clock()->now());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  std::vector{txn1.toBSON(), txn2.toBSON()});

    // Add some "new" sessions to ensure they don't get reaped
    clock()->advance(Minutes{31});
    _collectionMock->add(LogicalSessionRecord(makeLogicalSessionIdForTest(), clock()->now()));
    _collectionMock->add(LogicalSessionRecord(makeLogicalSessionIdForTest(), clock()->now()));

    auto mongoDSessionCatalog = MongoDSessionCatalog{};
    auto numReaped = mongoDSessionCatalog.reapSessionsOlderThan(
        _opCtx, *_collection, clock()->now() - Minutes{30});

    ASSERT_EQ(2, numReaped);
}

}  // namespace
}  // namespace mongo

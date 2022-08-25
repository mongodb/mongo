/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/transaction/internal_transactions_reap_service.h"
#include "mongo/db/transaction/internal_transactions_reap_service_gen.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"

namespace mongo {
namespace {

class InternalTransactionsReapServiceTest : public ServiceContextMongoDTest {
protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        const auto service = getServiceContext();
        _opCtxHolder = makeOperationContext();
        const auto opCtx = _opCtxHolder.get();

        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
        repl::ReplicationCoordinator::set(
            service,
            std::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings()));
        repl::createOplog(opCtx);

        ReplicaSetAwareServiceRegistry::get(service).onStartup(opCtx);

        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        MongoDSessionCatalog::set(
            service,
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->onStepUp(opCtx);

        // Lower the reap threshold for faster tests.
        _originalInternalSessionsReapThreshold = internalSessionsReapThreshold.load();
        internalSessionsReapThreshold.store(50);

        onStepUp();
    }

    void tearDown() override {
        internalSessionsReapThreshold.store(_originalInternalSessionsReapThreshold);
        _opCtxHolder.reset();
        ReplicaSetAwareServiceRegistry::get(getServiceContext()).onShutdown();
        ServiceContextMongoDTest::tearDown();
    }

    InternalTransactionsReapService* reapService() {
        return InternalTransactionsReapService::get(getServiceContext());
    }

    OperationContext* opCtx() const {
        return _opCtxHolder.get();
    }

    void onStepUp() {
        // Set a last applied opTime to satisfy an invariant in the primary only service registry.
        auto newLastAppliedOpTime = repl::OpTime(Timestamp(1, 1), 1);
        repl::ReplicationCoordinator::get(getServiceContext())
            ->setMyLastAppliedOpTimeAndWallTime({newLastAppliedOpTime, Date_t()});
        ReplicaSetAwareServiceRegistry::get(getServiceContext()).onStepUpComplete(opCtx(), 1LL);
    }

    void onStepDown() {
        ReplicaSetAwareServiceRegistry::get(getServiceContext()).onStepDown();
    }

    // Inserts entries for the given sessions into config.transactions and config.image_collection.
    void insertSessionDocuments(const std::vector<LogicalSessionId>& lsids,
                                bool skipImageEntry = false,
                                bool skipSessionEntry = false) {
        DBDirectClient client(opCtx());

        if (!skipImageEntry) {
            write_ops::checkWriteErrors(client.insert([&] {
                write_ops::InsertCommandRequest imageInsertOp(
                    NamespaceString::kConfigImagesNamespace);
                imageInsertOp.setWriteCommandRequestBase([] {
                    write_ops::WriteCommandRequestBase base;
                    base.setOrdered(false);
                    return base;
                }());
                imageInsertOp.setDocuments([&] {
                    std::vector<BSONObj> docs;
                    for (const auto& lsid : lsids) {
                        repl::ImageEntry entry(
                            lsid, 0, Timestamp(1, 1), repl::RetryImageEnum::kPreImage, BSONObj());
                        docs.emplace_back(entry.toBSON());
                    }
                    return docs;
                }());
                return imageInsertOp;
            }()));
        }

        if (!skipSessionEntry) {
            auto sessionDeleteReply = write_ops::checkWriteErrors(client.insert([&] {
                write_ops::InsertCommandRequest sessionInsertOp(
                    NamespaceString::kSessionTransactionsTableNamespace);
                sessionInsertOp.setWriteCommandRequestBase([] {
                    write_ops::WriteCommandRequestBase base;
                    base.setOrdered(false);
                    return base;
                }());
                sessionInsertOp.setDocuments([&] {
                    std::vector<BSONObj> docs;
                    for (const auto& lsid : lsids) {
                        SessionTxnRecord record(lsid, 0, repl::OpTime(), Date_t());
                        docs.emplace_back(record.toBSON());
                    }
                    return docs;
                }());
                return sessionInsertOp;
            }()));
        }
    }

    void assertInSessionsCollection(const std::vector<LogicalSessionId>& lsids) {
        DBDirectClient client(opCtx());
        for (const auto& lsid : lsids) {
            auto numImageEntries =
                client.count(NamespaceString::kConfigImagesNamespace,
                             BSON(repl::ImageEntry::k_idFieldName << lsid.toBSON()));
            ASSERT_EQ(1, numImageEntries) << lsid.toBSON();
            auto numSessionEntries =
                client.count(NamespaceString::kSessionTransactionsTableNamespace,
                             BSON(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON()));
            ASSERT_EQ(1, numSessionEntries) << lsid.toBSON();
        }
    }

    void assertNotInSessionsCollection(const std::vector<LogicalSessionId>& lsids) {
        DBDirectClient client(opCtx());
        for (const auto& lsid : lsids) {
            auto numImageEntries =
                client.count(NamespaceString::kConfigImagesNamespace,
                             BSON(repl::ImageEntry::k_idFieldName << lsid.toBSON()));
            ASSERT_EQ(0, numImageEntries) << lsid.toBSON();
            auto numSessionEntries =
                client.count(NamespaceString::kSessionTransactionsTableNamespace,
                             BSON(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON()));
            ASSERT_EQ(0, numSessionEntries) << lsid.toBSON();
        }
    }

    void assertNoPersistedSessions() {
        DBDirectClient client(opCtx());
        auto numImageEntries = client.count(NamespaceString::kConfigImagesNamespace, BSONObj());
        ASSERT_EQ(0, numImageEntries);
        auto numSessionEntries =
            client.count(NamespaceString::kSessionTransactionsTableNamespace, BSONObj());
        ASSERT_EQ(0, numSessionEntries);
    }

    std::vector<LogicalSessionId> generateLsids(int num) {
        std::vector<LogicalSessionId> lsids;
        for (int i = 0; i < num; i++) {
            lsids.push_back(makeLogicalSessionIdForTest());
        }
        return lsids;
    }

private:
    repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }

    int _originalInternalSessionsReapThreshold;
    ServiceContext::UniqueOperationContext _opCtxHolder;
};

TEST_F(InternalTransactionsReapServiceTest, DoesNotReapUntilThreshold) {
    auto lsidsBelowThreshold = generateLsids(internalSessionsReapThreshold.load() - 1);
    insertSessionDocuments(lsidsBelowThreshold);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsBelowThreshold);
    assertInSessionsCollection(lsidsBelowThreshold);

    auto lsidToReachThreshold = generateLsids(1);
    insertSessionDocuments(lsidToReachThreshold);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidToReachThreshold);
    reapService()->waitForCurrentDrain_forTest();
    assertNoPersistedSessions();
}

TEST_F(InternalTransactionsReapServiceTest, ExceedingThresholdTriggersReap) {
    auto lsidsBelowThreshold = generateLsids(internalSessionsReapThreshold.load() - 1);
    insertSessionDocuments(lsidsBelowThreshold);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsBelowThreshold);
    assertInSessionsCollection(lsidsBelowThreshold);

    // Passing the threshold by a large amount is fine and will trigger removing all accumulated
    // sessions, not just the threshold amount.
    auto lsidsToExceedThreshold = generateLsids(internalSessionsReapThreshold.load() * 2);
    insertSessionDocuments(lsidsToExceedThreshold);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsToExceedThreshold);
    reapService()->waitForCurrentDrain_forTest();
    assertNoPersistedSessions();
}

TEST_F(InternalTransactionsReapServiceTest, CanReapMultipleRounds) {
    auto reap = [&] {
        auto lsidsBelowThreshold = generateLsids(internalSessionsReapThreshold.load() - 1);
        insertSessionDocuments(lsidsBelowThreshold);
        reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsBelowThreshold);
        assertInSessionsCollection(lsidsBelowThreshold);

        auto lsidToReachThreshold = generateLsids(1);
        insertSessionDocuments(lsidToReachThreshold);
        reapService()->addEagerlyReapedSessions(getServiceContext(), lsidToReachThreshold);
        reapService()->waitForCurrentDrain_forTest();
        assertNoPersistedSessions();
    };

    // Reaps can be triggered multiple times.
    reap();
    reap();
    reap();
}

TEST_F(InternalTransactionsReapServiceTest, CanReapMoreThanMaxSessionDeletionBatchSize) {
    auto lsidsOverBatchSize =
        generateLsids(MongoDSessionCatalog::kMaxSessionDeletionBatchSize + 11);
    insertSessionDocuments(lsidsOverBatchSize);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsOverBatchSize);
    reapService()->waitForCurrentDrain_forTest();
    assertNoPersistedSessions();
}

TEST_F(InternalTransactionsReapServiceTest, OnlySchedulesOneReapAtATime) {
    auto pauseReaperThreadFp =
        globalFailPointRegistry().find("pauseInternalTransactionsReaperAfterSwap");
    auto timesEnteredFailPoint = pauseReaperThreadFp->setMode(FailPoint::alwaysOn);

    auto lsidsForInitialReap = generateLsids(internalSessionsReapThreshold.load());
    insertSessionDocuments(lsidsForInitialReap);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsForInitialReap);
    ASSERT(reapService()->hasCurrentDrain_forTest());
    assertInSessionsCollection(lsidsForInitialReap);

    // Wait for fail point.
    pauseReaperThreadFp->waitForTimesEntered(timesEnteredFailPoint + 1);

    auto lsidsDuringReap = generateLsids(internalSessionsReapThreshold.load());
    insertSessionDocuments(lsidsDuringReap);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsDuringReap);
    ASSERT(reapService()->hasCurrentDrain_forTest());
    assertInSessionsCollection(lsidsDuringReap);

    // Disable fail point and verify the concurrent sessions weren't reaped.
    pauseReaperThreadFp->setMode(FailPoint::off);
    reapService()->waitForCurrentDrain_forTest();
    assertNotInSessionsCollection(lsidsForInitialReap);
    assertInSessionsCollection(lsidsDuringReap);

    // The concurrently added sessions will be reaped in the next round.
    reapService()->addEagerlyReapedSessions(getServiceContext(), generateLsids(1));
    reapService()->waitForCurrentDrain_forTest();
    assertNoPersistedSessions();
};

TEST_F(InternalTransactionsReapServiceTest, DoesNotErrorIfReceivedSameSessionTwice) {
    auto lsids = generateLsids(2);
    insertSessionDocuments(lsids);

    reapService()->addEagerlyReapedSessions(getServiceContext(), lsids);
    assertInSessionsCollection(lsids);

    reapService()->addEagerlyReapedSessions(getServiceContext(), lsids);
    assertInSessionsCollection(lsids);

    auto lsidsToReachThreshold = generateLsids(internalSessionsReapThreshold.load());
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsToReachThreshold);
    reapService()->waitForCurrentDrain_forTest();
    assertNoPersistedSessions();
}

TEST_F(InternalTransactionsReapServiceTest, DoesNotErrorIfSessionOrImageIsNotOnDisk) {
    auto lsidsNoImage = generateLsids(2);
    insertSessionDocuments(lsidsNoImage, true /* skipImageEntry */);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsNoImage);

    auto lsidsNoSession = generateLsids(2);
    insertSessionDocuments(lsidsNoSession, false /* skipImageEntry */, true /* skipSessionEntry */);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsNoSession);

    auto normalLsids = generateLsids(3);
    insertSessionDocuments(normalLsids);
    reapService()->addEagerlyReapedSessions(getServiceContext(), normalLsids);

    auto lsidsNoImageOrSession = generateLsids(internalSessionsReapThreshold.load());
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsNoImageOrSession);

    reapService()->waitForCurrentDrain_forTest();
    assertNoPersistedSessions();
}

TEST_F(InternalTransactionsReapServiceTest, DoesNotReapAsSecondaryAndClearsSessionsOnStepdown) {
    auto lsidsAsPrimary = generateLsids(2);
    insertSessionDocuments(lsidsAsPrimary);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsAsPrimary);
    assertInSessionsCollection(lsidsAsPrimary);

    onStepDown();

    auto lsidsAsSecondary = generateLsids(internalSessionsReapThreshold.load());
    insertSessionDocuments(lsidsAsSecondary);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsAsSecondary);
    ASSERT_FALSE(reapService()->hasCurrentDrain_forTest());
    assertInSessionsCollection(lsidsAsPrimary);
    assertInSessionsCollection(lsidsAsSecondary);

    onStepUp();

    // Despite having seen more than the threshold as a secondary and previous primary, there should
    // have been no sessions buffered so this will not trigger a refresh.
    auto newLsidAsPrimary = generateLsids(1);
    insertSessionDocuments(newLsidAsPrimary);
    reapService()->addEagerlyReapedSessions(getServiceContext(), newLsidAsPrimary);
    ASSERT_FALSE(reapService()->hasCurrentDrain_forTest());
    assertInSessionsCollection(lsidsAsPrimary);
    assertInSessionsCollection(lsidsAsSecondary);
    assertInSessionsCollection(newLsidAsPrimary);

    auto lsidsToReachThreshold = generateLsids(internalSessionsReapThreshold.load());
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsToReachThreshold);
    reapService()->waitForCurrentDrain_forTest();

    // The previous sessions were missed and will remain until the normal logical session reaper
    // logic removes them.
    assertInSessionsCollection(lsidsAsPrimary);
    assertInSessionsCollection(lsidsAsSecondary);

    // But newly added sessions will be reaped.
    assertNotInSessionsCollection(newLsidAsPrimary);
    assertNotInSessionsCollection(lsidsToReachThreshold);
}

TEST_F(InternalTransactionsReapServiceTest, ThresholdOfZeroDisablesReaping) {
    // tearDown() will restore the original value already, so there's no need to do it in the test.
    internalSessionsReapThreshold.store(0);

    auto lsidsOverZeroThreshold = generateLsids(10);
    insertSessionDocuments(lsidsOverZeroThreshold);
    reapService()->addEagerlyReapedSessions(getServiceContext(), lsidsOverZeroThreshold);
    ASSERT_FALSE(reapService()->hasCurrentDrain_forTest());
    assertInSessionsCollection(lsidsOverZeroThreshold);
}

}  // namespace
}  // namespace mongo

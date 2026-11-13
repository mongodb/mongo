// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/sbe/stages/exchange_worker_pool.h"
#include "mongo/db/exec/sbe/stages/shared_read_view.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/shard_role/shard_catalog/allow_read_from_latest_on_secondary.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::sbe {
namespace {

/**
 * Verifies that the OperationContexts of the workers leased from the ExchangeWorkerPool read what
 * the "Read concern levels" table of the parallel query design specifies for their main operation.
 * Every test reads a collection holding documents committed at increasing timestamps, and moves the
 * point that decides what a read sees (lastApplied, the majority commit point, ...) between reads.
 */
class ExchangeWorkerReadViewTest : public CatalogTestFixture {
public:
    // Commit timestamps of the documents {_id: 1}, {_id: 2} and {_id: 3}, and the prepare timestamp
    // of the transaction started by PreparedInsert.
    static inline const Timestamp kCreateTs{5, 1};
    static inline const Timestamp kT1{10, 1};
    static inline const Timestamp kT2{20, 1};
    static inline const Timestamp kT3{30, 1};
    static inline const Timestamp kPrepareTs{40, 1};

    Options makeServiceContextOptions() override {
        // Without replication settings every table is logged, and WiredTiger ignores the timestamps
        // of the writes to logged tables, which would make them visible at any read timestamp.
        return CatalogTestFixture::makeServiceContextOptions().forceDisableTableLogging();
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        {
            // Point-in-time reads only find collections whose creation is timestamped at or before
            // their read timestamp.
            repl::UnreplicatedWritesBlock unreplicated(operationContext());
            TimestampBlock tsBlock(operationContext(), kCreateTs);
            ASSERT_OK(storageInterface()->createCollection(operationContext(), _nss, {}));
        }
        insertAt(BSON("_id" << 1), kT1);
        insertAt(BSON("_id" << 2), kT2);
        insertAt(BSON("_id" << 3), kT3);
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    }

    void insertAt(const BSONObj& doc, Timestamp ts) {
        ASSERT_OK(storageInterface()->insertDocument(operationContext(), _nss, {doc, ts}, 1));
    }

    OperationContext* mainOpCtx() {
        return operationContext();
    }

    RecoveryUnit* mainRu() {
        return shard_role_details::getRecoveryUnit(mainOpCtx());
    }

    SnapshotManager* snapshotManager() {
        return getServiceContext()->getStorageEngine()->getSnapshotManager();
    }

    void setReadConcern(OperationContext* opCtx, repl::ReadConcernArgs rc) {
        std::lock_guard<Client> lk(*opCtx->getClient());
        repl::ReadConcernArgs::get(opCtx) = std::move(rc);
    }

    std::vector<ExchangeWorkerPool::Handle> lease(size_t count) {
        return ExchangeWorkerPool::get(getServiceContext()).acquire(mainOpCtx(), count);
    }

    /**
     * Reads the _ids of the collection on the worker's thread and OperationContext, as a producer
     * of a parallel scan would, then abandons the snapshot like a yield does, so that the next read
     * opens a new one.
     */
    std::set<int> readOnWorker(ExchangeWorker& worker) {
        return worker
            .submitAsPromise([&] {
                auto* opCtx = worker.opCtxHolder.get();
                std::set<int> ids;
                {
                    const auto coll = acquireCollection(
                        opCtx,
                        CollectionAcquisitionRequest::fromOpCtx(
                            opCtx, _nss, AcquisitionPrerequisites::OperationType::kRead),
                        MODE_IS);
                    auto cursor = coll.getCollectionPtr()->getCursor(opCtx);
                    while (auto record = cursor->next()) {
                        ids.insert(record->data.toBson()["_id"].numberInt());
                    }
                }
                shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
                return ids;
            })
            .get();
    }

    /**
     * Inserts {_id: 4} in a transaction prepared at kPrepareTs, which is aborted on destruction.
     * The insert goes straight to the record store, on a RecoveryUnit of its own.
     */
    class PreparedInsert {
    public:
        PreparedInsert(OperationContext* opCtx, const NamespaceString& nss)
            : _writer(makeWriter(opCtx)), _txn(*_writer) {
            const auto doc = BSON("_id" << 4);
            auto* rs = CollectionCatalog::get(opCtx)
                           ->lookupCollectionByNamespace(opCtx, nss)
                           ->getRecordStore();
            ASSERT_OK(rs->insertRecord(opCtx, *_writer, doc.objdata(), doc.objsize(), Timestamp())
                          .getStatus());
            _writer->setPrepareTimestamp(kPrepareTs);
            _txn.prepare();
        }

        ~PreparedInsert() {
            _txn.abort();
        }

    private:
        static std::unique_ptr<RecoveryUnit> makeWriter(OperationContext* opCtx) {
            auto writer = opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit();
            // The record store registers rollback handlers, which run with an OperationContext.
            writer->setOperationContext(opCtx);
            return writer;
        }

        std::unique_ptr<RecoveryUnit> _writer;
        StorageWriteTransaction _txn;
    };

    /**
     * Checks that no worker is leased for the main operation, for 'expectedReason', which is also
     * logged.
     */
    void assertNoWorkerLeased(std::string_view expectedReason) {
        const auto readView = SharedReadView::capture(mainOpCtx());
        ASSERT_EQ(readView.getStatus().code(), ErrorCodes::IllegalOperation);
        ASSERT_EQ(readView.getStatus().reason(), expectedReason);

        auto& pool = ExchangeWorkerPool::get(getServiceContext());
        const auto leasedBefore = pool.leasedCount();
        unittest::MinimumLoggedSeverityGuard logSeverityGuard{logv2::LogComponent::kQuery,
                                                              logv2::LogSeverity::Debug(1)};
        unittest::LogCaptureGuard logs;
        ASSERT_TRUE(lease(2).empty());
        logs.stop();
        ASSERT_EQ(pool.leasedCount(), leasedBefore);
        ASSERT_EQ(
            1,
            logs.countBSONContainingSubset(
                BSON("id" << 13436703 << "attr" << BSON("reason" << std::string{expectedReason}))));
    }

    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test.coll");
};

TEST_F(ExchangeWorkerReadViewTest, LocalReadOnPrimaryReadsLatestData) {
    auto workers = lease(2);
    ASSERT_EQ(workers.size(), 2u);
    for (auto& worker : workers) {
        ASSERT_EQ(shard_role_details::getRecoveryUnit(worker->opCtxHolder.get())
                      ->getTimestampReadSource(),
                  RecoveryUnit::ReadSource::kNoTimestamp);
        ASSERT_EQ(readOnWorker(*worker), (std::set<int>{1, 2, 3}));
    }

    // A document committed while the workers are leased is seen by their next snapshot.
    insertAt(BSON("_id" << 5), Timestamp(50, 1));
    for (auto& worker : workers) {
        ASSERT_EQ(readOnWorker(*worker), (std::set<int>{1, 2, 3, 5}));
    }
}

TEST_F(ExchangeWorkerReadViewTest, SnapshotReadReadsAtTheMainOperationTimestamp) {
    setReadConcern(mainOpCtx(), repl::ReadConcernArgs::snapshot(kT1));
    mainRu()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, kT1);

    auto workers = lease(2);
    ASSERT_EQ(workers.size(), 2u);
    for (auto& worker : workers) {
        auto* workerOpCtx = worker->opCtxHolder.get();
        ASSERT_EQ(repl::ReadConcernArgs::get(workerOpCtx).getLevel(),
                  repl::ReadConcernLevel::kSnapshotReadConcern);
        ASSERT_EQ(*repl::ReadConcernArgs::get(workerOpCtx).getArgsAtClusterTime(),
                  LogicalTime(kT1));
        // Every snapshot of the worker, including those reopened after a yield, is at kT1.
        ASSERT_EQ(readOnWorker(*worker), (std::set<int>{1}));
        ASSERT_EQ(readOnWorker(*worker), (std::set<int>{1}));
    }
    workers.clear();

    // So is every snapshot of the workers leased by the next getMore.
    workers = lease(2);
    for (auto& worker : workers) {
        ASSERT_EQ(readOnWorker(*worker), (std::set<int>{1}));
    }
}

TEST_F(ExchangeWorkerReadViewTest, MajorityReadResolvesTheCommitPointAtEachWorkerSnapshot) {
    snapshotManager()->setCommittedSnapshot(kT1);
    setReadConcern(mainOpCtx(),
                   repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern));
    mainRu()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);

    auto workers = lease(2);
    ASSERT_EQ(workers.size(), 2u);
    ASSERT_EQ(readOnWorker(*workers[0]), (std::set<int>{1}));

    // The next snapshot of each worker sees the new majority commit point, whether or not that
    // worker read before it moved.
    snapshotManager()->setCommittedSnapshot(kT2);
    for (auto& worker : workers) {
        ASSERT_EQ(readOnWorker(*worker), (std::set<int>{1, 2}));
    }
}

TEST_F(ExchangeWorkerReadViewTest, LocalReadOnSecondaryReadsAtLastApplied) {
    ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));
    snapshotManager()->setLastApplied(kT1);

    auto workers = lease(2);
    ASSERT_EQ(workers.size(), 2u);
    for (auto& worker : workers) {
        ASSERT_EQ(readOnWorker(*worker), (std::set<int>{1}));
    }

    // Data committed by an oplog batch beyond lastApplied is not visible until lastApplied moves.
    snapshotManager()->setLastApplied(kT2);
    for (auto& worker : workers) {
        ASSERT_EQ(readOnWorker(*worker), (std::set<int>{1, 2}));
    }
}

TEST_F(ExchangeWorkerReadViewTest, WorkersIgnorePrepareConflictsWhenTheMainOperationDoes) {
    mainRu()->setPrepareConflictBehavior(PrepareConflictBehavior::ignoreAndAllowWrites);
    PreparedInsert prepared(mainOpCtx(), _nss);
    // A read that enforces prepare conflicts fails rather than waiting for the prepared
    // transaction to commit or abort.
    FailPointEnableBlock skipRetries("WTSkipPrepareConflictRetries");

    auto workers = lease(2);
    ASSERT_EQ(workers.size(), 2u);
    for (auto& worker : workers) {
        ASSERT_EQ(readOnWorker(*worker), (std::set<int>{1, 2, 3}));
    }
}

TEST_F(ExchangeWorkerReadViewTest, WorkersEnforcePrepareConflictsWhenTheMainOperationDoes) {
    mainRu()->setPrepareConflictBehavior(PrepareConflictBehavior::block);
    PreparedInsert prepared(mainOpCtx(), _nss);
    FailPointEnableBlock skipRetries("WTSkipPrepareConflictRetries");

    auto workers = lease(1);
    ASSERT_EQ(workers.size(), 1u);
    ASSERT_THROWS_CODE(readOnWorker(*workers[0]), DBException, ErrorCodes::WriteConflict);
}

TEST_F(ExchangeWorkerReadViewTest, WorkersInheritReadOnceAndEnforceConstraints) {
    mainRu()->setReadOnce(true);
    mainOpCtx()->setEnforceConstraints(false);

    auto workers = lease(1);
    ASSERT_EQ(workers.size(), 1u);
    auto* workerOpCtx = workers[0]->opCtxHolder.get();
    ASSERT_TRUE(shard_role_details::getRecoveryUnit(workerOpCtx)->getReadOnce());
    ASSERT_FALSE(workerOpCtx->isEnforcingConstraints());
}

TEST_F(ExchangeWorkerReadViewTest, ReusedWorkerTakesTheViewOfItsNewMainOperation) {
    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    setReadConcern(mainOpCtx(), repl::ReadConcernArgs::snapshot(kT1));
    mainRu()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, kT1);
    lease(1).clear();
    const auto created = pool.createdCount();

    setReadConcern(mainOpCtx(), repl::ReadConcernArgs());
    mainRu()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    auto workers = lease(1);
    ASSERT_EQ(workers.size(), 1u);
    ASSERT_EQ(pool.createdCount(), created);
    ASSERT_EQ(readOnWorker(*workers[0]), (std::set<int>{1, 2, 3}));
}

TEST_F(ExchangeWorkerReadViewTest, WorkersInheritTheDeadline) {
    // A main operation of its own, so that the fixture's OperationContext keeps no deadline.
    auto client = getService()->makeClient("main");
    AlternativeClientRegion acr(client);
    auto opCtx = cc().makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Seconds(100), ErrorCodes::MaxTimeMSExpired);

    auto workers = ExchangeWorkerPool::get(getServiceContext()).acquire(opCtx.get(), 1);
    ASSERT_EQ(workers.size(), 1u);
    auto* workerOpCtx = workers[0]->opCtxHolder.get();
    ASSERT_EQ(workerOpCtx->getDeadline(), opCtx->getDeadline());
    ASSERT_EQ(workerOpCtx->getTimeoutError(), ErrorCodes::MaxTimeMSExpired);
}

TEST_F(ExchangeWorkerReadViewTest, WorkerWaitingOnAPrepareConflictTimesOutWithTheMainOperation) {
    PreparedInsert prepared(operationContext(), _nss);

    auto client = getService()->makeClient("main");
    AlternativeClientRegion acr(client);
    auto opCtx = cc().makeOperationContext();
    shard_role_details::getRecoveryUnit(opCtx.get())
        ->setPrepareConflictBehavior(PrepareConflictBehavior::block);
    opCtx->setDeadlineAfterNowBy(Milliseconds(100), ErrorCodes::MaxTimeMSExpired);

    auto workers = ExchangeWorkerPool::get(getServiceContext()).acquire(opCtx.get(), 1);
    ASSERT_EQ(workers.size(), 1u);
    auto* workerOpCtx = workers[0]->opCtxHolder.get();

    // Without a deadline the worker would wait for the prepared transaction forever: interrupt it
    // after a while, so that the test fails rather than hangs.
    std::mutex mutex;
    stdx::condition_variable cv;
    bool done = false;
    stdx::thread watchdog([&] {
        std::unique_lock lk(mutex);
        if (!cv.wait_for(lk, std::chrono::seconds(30), [&] { return done; })) {
            ClientLock clientLock(workerOpCtx->getClient());
            getServiceContext()->killOperation(clientLock, workerOpCtx, ErrorCodes::Interrupted);
        }
    });
    ScopeGuard stopWatchdog([&] {
        {
            std::lock_guard lk(mutex);
            done = true;
        }
        cv.notify_all();
        watchdog.join();
    });

    ASSERT_THROWS_CODE(readOnWorker(*workers[0]), DBException, ErrorCodes::MaxTimeMSExpired);
}

TEST_F(ExchangeWorkerReadViewTest, NoWorkerForLinearizableReads) {
    setReadConcern(mainOpCtx(),
                   repl::ReadConcernArgs(repl::ReadConcernLevel::kLinearizableReadConcern));
    assertNoWorkerLeased("linearizable read concern");
}

TEST_F(ExchangeWorkerReadViewTest, NoWorkerInsideAWriteUnitOfWork) {
    WriteUnitOfWork wuow(mainOpCtx());
    assertNoWorkerLeased("unit of work in progress");
}

TEST_F(ExchangeWorkerReadViewTest, NoWorkerInsideAStorageWriteTransaction) {
    // A unit of work opened on the RecoveryUnit directly, which the Locker does not know about.
    StorageWriteTransaction txn(*mainRu());
    assertNoWorkerLeased("unit of work in progress");
}

TEST_F(ExchangeWorkerReadViewTest, NoWorkerForALocalReadOnSecondaryAllowedToReadFromLatest) {
    ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));
    snapshotManager()->setLastApplied(kT1);
    // The main operation reads the latest data, where workers would read at lastApplied.
    AllowReadFromLatestOnSecondaryBlock_UNSAFE allowReadFromLatest(mainOpCtx());
    assertNoWorkerLeased("reads the latest data on a secondary");
}

TEST_F(ExchangeWorkerReadViewTest, NoWorkerForAPinnedReadSource) {
    PinReadSourceBlock pinned(mainRu());
    assertNoWorkerLeased("pinned read source");
}

TEST_F(ExchangeWorkerReadViewTest, NoWorkerForSpeculativeMajorityReads) {
    mainRu()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
    assertNoWorkerLeased("kNoOverlap read source");
}

TEST_F(ExchangeWorkerReadViewTest, NoWorkerInsideAMultiDocumentTransaction) {
    auto client = getService()->makeClient("transaction");
    AlternativeClientRegion acr(client);
    auto opCtx = cc().makeOperationContext();
    opCtx->setInMultiDocumentTransaction();

    const auto readView = SharedReadView::capture(opCtx.get());
    ASSERT_EQ(readView.getStatus().code(), ErrorCodes::IllegalOperation);
    ASSERT_EQ(readView.getStatus().reason(), "multi-document transaction");
    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    ASSERT_TRUE(pool.acquire(opCtx.get(), 2).empty());
    ASSERT_EQ(pool.leasedCount(), 0u);
}

}  // namespace
}  // namespace mongo::sbe

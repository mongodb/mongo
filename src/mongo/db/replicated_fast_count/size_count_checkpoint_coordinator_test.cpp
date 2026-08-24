// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_coordinator.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_oplog_tailer.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

namespace mongo::replicated_fast_count {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class SizeCountCheckpointCoordinatorTest : public CatalogTestFixture {
public:
    SizeCountCheckpointCoordinatorTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();

        auto stores = test_helpers::createContainerFastCountStores(_opCtx);
        _sizeCountStore = std::move(stores.sizeCountStore);
        _timestampStore = std::move(stores.timestampStore);

        _coordinator = std::make_unique<SizeCountCheckpointCoordinator>(
            *_sizeCountStore, *_timestampStore, oplogUuid(), Timestamp::min());
    }

    UUID oplogUuid() const {
        AutoGetOplogFastPath oplogRead(_opCtx, OplogAccessMode::kRead);
        return oplogRead.getCollection()->uuid();
    }

    void tearDown() override {
        _coordinator.reset();
        CatalogTestFixture::tearDown();
    }

    boost::optional<Timestamp> readTimestampStore() {
        Lock::GlobalLock lk(_opCtx, MODE_IS);
        return _timestampStore->read(_opCtx);
    }

    OperationContext* _opCtx = nullptr;
    std::unique_ptr<ContainerSizeCountStore> _sizeCountStore;
    std::unique_ptr<ContainerSizeCountTimestampStore> _timestampStore;
    std::unique_ptr<SizeCountCheckpointCoordinator> _coordinator;
};

TEST_F(SizeCountCheckpointCoordinatorTest, MultipleRequestFlushCallsBeforeFlushAreCoalesced) {
    // _flushRequested is a bool: N calls collapse to a single pending flush.
    for (int i = 0; i < 10; ++i) {
        _coordinator->requestFlush();
    }
    ASSERT_TRUE(_coordinator->isFlushRequested_ForTest());
}

TEST_F(SizeCountCheckpointCoordinatorTest, FlushSyncWithEmptyTimestampStoreIsNoOp) {
    const auto initial = readTimestampStore();
    _coordinator->flushSync_ForTest(_opCtx);
    ASSERT_EQ(readTimestampStore(), initial);
}

class SizeCountCheckpointCoordinatorWithOplogTest : public SizeCountCheckpointCoordinatorTest {
protected:
    void setUp() override {
        SizeCountCheckpointCoordinatorTest::setUp();
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), _collA.nss, CollectionOptions{.uuid = _collA.uuid}));
    }

    void writeInsert(Timestamp ts, int32_t sizeDelta = 10) {
        test_helpers::writeToOplog(
            operationContext(),
            test_helpers::makeOplogEntry(ts, _collA, repl::OpTypeEnum::kInsert, sizeDelta));
        repl::signalOplogWaiters();
    }

    const test_helpers::NsAndUUID _collA{
        .nss = NamespaceString::createNamespaceString_forTest("coordinator_test", "collA"),
        .uuid = UUID::gen(),
    };
};

TEST_F(SizeCountCheckpointCoordinatorWithOplogTest,
       FlushSyncWithNoNewDataPreservesPersistedTimestamp) {
    const Timestamp persistedTs(10, 5);
    {
        Lock::GlobalLock writeLock(_opCtx, MODE_IX);
        WriteUnitOfWork wuow(_opCtx);
        _timestampStore->write(_opCtx, persistedTs);
        wuow.commit();
    }

    // Write an oplog entry at persistedTs so bootstrap can seekExact to it (simulating that the
    // previous checkpoint run had processed up to this point and the entry is still in the oplog).
    writeInsert(persistedTs);

    auto coordinator = std::make_unique<SizeCountCheckpointCoordinator>(
        *_sizeCountStore, *_timestampStore, oplogUuid(), persistedTs);
    coordinator->flushSync_ForTest(_opCtx);

    ASSERT_EQ(readTimestampStore(), boost::optional<Timestamp>(persistedTs));
}

TEST_F(SizeCountCheckpointCoordinatorWithOplogTest, FlushSyncAdvancesTimestampAfterTailCycle) {
    const Timestamp ts(10, 1);
    writeInsert(ts);

    _coordinator->flushSync_ForTest(_opCtx);

    ASSERT_EQ(readTimestampStore(), boost::optional<Timestamp>(ts));
}

TEST_F(SizeCountCheckpointCoordinatorTest, FlushFailureIncrementsFlushFailureCountMetric) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "Skipping test due to OTel metrics being unavailable in this build";
    }

    // The failure metric is incremented by the flush thread's run() loop, which the synchronous
    // flushSync_ForTest path does not exercise, so drive the real flush thread.
    FailPointEnableBlock failFp("failDuringFlush");
    _coordinator->startup(getServiceContext());
    _coordinator->requestFlush();

    // Wait until the flush has reached the failpoint, so destruction cannot preempt the flush
    // before run()'s catch classifies the failure and increments the metric.
    failFp->waitForTimesEntered(failFp.initialTimesEntered() + 1);

    // The destructor joins the flush thread. The join cannot return until the thread has run
    // run()'s catch (incrementing the metric) and exited, so the assertion needs no polling.
    _coordinator.reset();

    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushFailureCount), 1);

    // A failed flush does not increment success metrics.
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushSuccessCount), 0);
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushedDocsTotal), 0);
}

TEST_F(SizeCountCheckpointCoordinatorTest,
       DestructorJoinsBackgroundThreadsWithoutExplicitShutdown) {
    {
        auto localCoord = std::make_unique<SizeCountCheckpointCoordinator>(
            *_sizeCountStore, *_timestampStore, oplogUuid(), Timestamp::min());
        localCoord->startup(getServiceContext());
        // Destructor joins the worker threads.
    }
    // Reaching here without a hang or crash confirms the destructor joined successfully.
}

TEST_F(SizeCountCheckpointCoordinatorTest, DestructorDuringFlushCycleInterruptsAndCompletes) {
    OtelMetricsCapturer capturer;
    _coordinator->startup(getServiceContext());

    stdx::thread destroyer;
    {
        FailPointEnableBlock hangFp("hangAfterReplicatedFastCountSnapshot");

        _coordinator->requestFlush();
        hangFp->waitForTimesEntered(hangFp.initialTimesEntered() + 1);

        // Destroy the coordinator while the flush thread is stalled inside _runOneFlushCycle.
        // The destructor interrupts the flush thread's opCtx, but the thread cannot unblock
        // until the failpoint is disabled (pauseWhileSet does not check the opCtx).
        destroyer = stdx::thread([&] { _coordinator.reset(); });

        // Scope exit: disables failpoint. The flush thread then observes the interrupted opCtx
        // and surfaces InterruptedDueToReplStateChange, which run() treats as a benign
        // replication-state change (not a flush failure) before exiting the loop.
    }

    destroyer.join();

    // The destruction interrupt is a replication-state change, not a flush failure.
    if (capturer.canReadMetrics()) {
        ASSERT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushFailureCount), 0);
    }
}

TEST_F(SizeCountCheckpointCoordinatorTest, ConcurrentRequestFlushAndDestructorNeverDeadlocks) {
    for (int i = 0; i < 50; ++i) {
        auto coordinator = std::make_shared<SizeCountCheckpointCoordinator>(
            *_sizeCountStore, *_timestampStore, oplogUuid(), Timestamp::min());
        coordinator->startup(getServiceContext());

        // The flusher thread holds a copy of the shared_ptr to keep the coordinator alive
        // while it calls requestFlush(). The main thread releases its copy concurrently,
        // which may trigger the destructor if the flusher has already finished.
        stdx::thread flusher([coordinator] {
            for (int j = 0; j < 5; ++j) {
                coordinator->requestFlush();
            }
        });
        stdx::thread destroyer([&] { coordinator.reset(); });

        flusher.join();
        destroyer.join();
    }
}

/**
 * Round-trip coverage for the validation hash over the production flush path: the tailer scans the
 * oplog, the buffer accumulates, and `SizeCountCheckpointFlusher::_doFlush()` persists.
 */
class SizeCountCheckpointCoordinatorHashTest : public CatalogTestFixture {
public:
    SizeCountCheckpointCoordinatorHashTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    // The three hashes overlap in their set bits, so an expectation written as an XOR of them does
    // not also hold for a sum or a bitwise or. `kHashB` has its high bit set so a value that does
    // not survive the int64 encoding on the wire or on disk is caught.
    static constexpr int64_t kHashA = 0x0123456789ABCDEF;
    static constexpr int64_t kHashB = static_cast<int64_t>(0xF0E1D2C3B4A59687);
    static constexpr int64_t kHashC = 0x00FF00FF00FF00FF;

    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();
        auto stores = test_helpers::createContainerFastCountStores(_opCtx);
        _sizeCountStore = std::move(stores.sizeCountStore);
        _timestampStore = std::move(stores.timestampStore);
        _coordinator = std::make_unique<SizeCountCheckpointCoordinator>(
            *_sizeCountStore, *_timestampStore, oplogUuid(), Timestamp::min());
    }

    void tearDown() override {
        _coordinator.reset();
        CatalogTestFixture::tearDown();
    }

    UUID oplogUuid() const {
        AutoGetOplogFastPath oplogRead(_opCtx, OplogAccessMode::kRead);
        return oplogRead.getCollection()->uuid();
    }

    // Runs one tail-then-flush cycle: the tailer buffers everything newly visible in the oplog and
    // the flusher persists it.
    void flush() {
        _coordinator->flushSync_ForTest(_opCtx);
    }

    void writeOplogEntry(const repl::OplogEntry& entry) {
        test_helpers::writeToOplog(_opCtx, entry);
        repl::signalOplogWaiters();
    }

    // Writes an oplog entry for an insert, update, or delete on 'coll' carrying 'sizeDelta' and
    // 'hash' in its size metadata.
    void writeCrudEntry(Timestamp ts,
                        const test_helpers::NsAndUUID& coll,
                        repl::OpTypeEnum opType,
                        int32_t sizeDelta,
                        boost::optional<int64_t> hash) {
        writeOplogEntry(test_helpers::makeOplogEntry(ts, coll, opType, sizeDelta, hash));
    }

    // Builds one applyOps entry of a chained (unprepared) transaction on '_collA', holding a single
    // insert of 10 bytes whose size metadata carries 'hash'.
    repl::OplogEntry makeChainedApplyOps(Timestamp ts,
                                         int64_t hash,
                                         repl::OpTime prevOpTime,
                                         bool isPartialTxn = true) {
        BSONObjBuilder oBuilder;
        {
            BSONArrayBuilder innerOpsBuilder(oBuilder.subarrayStart("applyOps"));
            innerOpsBuilder.append(BSON("op" << "i"
                                             << "ns" << _collA.nss.ns_forTest() << "ui"
                                             << _collA.uuid << "o"
                                             << BSON("_id" << static_cast<int>(ts.getInc())) << "m"
                                             << BSON("sz" << 10 << "h" << hash)));
        }
        if (isPartialTxn) {
            oBuilder.append("partialTxn", true);
        }
        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = NamespaceString::kAdminCommandNamespace,
            .oField = oBuilder.obj(),
            .wallClockTime = Date_t::now(),
            .prevWriteOpTimeInTransaction = prevOpTime,
        }};
    }

    boost::optional<SizeCountStore::Entry> readSizeCount(UUID uuid) {
        Lock::GlobalLock lk(_opCtx, MODE_IS);
        return _sizeCountStore->read(_opCtx, uuid);
    }

    const test_helpers::NsAndUUID _collA{
        .nss = NamespaceString::createNamespaceString_forTest("coordinator_hash_test", "collA"),
        .uuid = UUID::gen(),
    };
    const test_helpers::NsAndUUID _collB{
        .nss = NamespaceString::createNamespaceString_forTest("coordinator_hash_test", "collB"),
        .uuid = UUID::gen(),
    };

    OperationContext* _opCtx = nullptr;
    std::unique_ptr<ContainerSizeCountStore> _sizeCountStore;
    std::unique_ptr<ContainerSizeCountTimestampStore> _timestampStore;
    std::unique_ptr<SizeCountCheckpointCoordinator> _coordinator;
};

// The `h` on a single oplog entry is parsed out of the oplog and lands in the persisted
// entry. The oplog collection's own entry carries no hash, since its deltas are byte counts of
// records rather than document contributions.
TEST_F(SizeCountCheckpointCoordinatorHashTest, HashOnOplogEntryPersistedOnCheckpoint) {
    const Timestamp ts{1, 1};
    writeCrudEntry(ts, _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = ts, .size = 10, .count = 1, .hash = kHashA};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);

    const auto oplogEntry = readSizeCount(oplogUuid());
    ASSERT_TRUE(oplogEntry.has_value());
    EXPECT_FALSE(oplogEntry->hash.has_value());
}

// The hashes on the entries scanned within one checkpoint are XOR-folded into the persisted
// hash.
TEST_F(SizeCountCheckpointCoordinatorHashTest, HashesWithinCheckpointAreXorFolded) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);
    writeCrudEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 20 /*sizeDelta=*/, kHashB);
    // An update's `h` is already the pre-image XOR the post-image, so it folds in like any other
    // contribution and leaves the count unchanged.
    writeCrudEntry(Timestamp(1, 3), _collA, repl::OpTypeEnum::kUpdate, 5 /*sizeDelta=*/, kHashC);

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 3), .size = 35, .count = 2, .hash = kHashA ^ kHashB ^ kHashC};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// Deleting the document that was inserted folds its contribution back out, leaving the hash
// of an empty collection. The persisted hash is 0, which is distinct from an absent hash.
TEST_F(SizeCountCheckpointCoordinatorHashTest, InsertThenDeleteFoldsBackToEmptyCollectionHash) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);
    writeCrudEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kDelete, -10 /*sizeDelta=*/, kHashA);

    flush();

    const SizeCountStore::Entry expectedEntry{.timestamp = Timestamp(1, 2),
                                              .size = 0,
                                              .count = 0,
                                              .hash = kEmptyCollectionValidationHash};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// An entry that carries a size delta but no `h` invalidates the hash for the whole
// checkpoint, so no hash is persisted even though the size and count still are.
TEST_F(SizeCountCheckpointCoordinatorHashTest, EntryWithoutHashInvalidatesPersistedHash) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);
    writeCrudEntry(
        Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 20 /*sizeDelta=*/, boost::none);

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 2), .size = 30, .count = 2, .hash = boost::none};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// A later checkpoint folds its hashes into the one already persisted rather than replacing
// it.
TEST_F(SizeCountCheckpointCoordinatorHashTest, HashFoldsIntoPersistedHashAcrossCheckpoints) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);

    flush();
    {
        const auto entry = readSizeCount(_collA.uuid);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->hash, kHashA);
    }

    writeCrudEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 20 /*sizeDelta=*/, kHashB);

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 2), .size = 30, .count = 2, .hash = kHashA ^ kHashB};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// A checkpoint that cannot account for every contribution clears the hash a previous
// checkpoint persisted, so a stale value is never left behind as if it were still valid.
TEST_F(SizeCountCheckpointCoordinatorHashTest, MissingHashInLaterCheckpointClearsPersistedHash) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);

    flush();

    writeCrudEntry(
        Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 20 /*sizeDelta=*/, boost::none);

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 2), .size = 30, .count = 2, .hash = boost::none};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// Once a collection's persisted hash has been invalidated, later checkpoints do not resume
// tracking it, because a hash folded over only part of the collection's history is not usable.
TEST_F(SizeCountCheckpointCoordinatorHashTest, InvalidatedHashIsNotResumedByLaterCheckpoint) {
    writeCrudEntry(
        Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, boost::none);

    flush();

    writeCrudEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 20 /*sizeDelta=*/, kHashA);

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 2), .size = 30, .count = 2, .hash = boost::none};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// A collection that was already being tracked before hash validation existed never starts
// reporting a hash, however many hash-carrying entries are checkpointed for it afterwards. A hash
// folded over only the entries seen since the upgrade would not describe the collection's full
// contents, so it must stay absent until something reseeds it.
TEST_F(SizeCountCheckpointCoordinatorHashTest, HashStaysAbsentForCollectionTrackedBeforeHashes) {
    test_helpers::insertSizeCountEntry(
        _opCtx,
        *_sizeCountStore,
        _collA.uuid,
        SizeCountStore::Entry{
            .timestamp = Timestamp(1, 1), .size = 100, .count = 3, .hash = boost::none});

    writeCrudEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);

    flush();
    {
        const SizeCountStore::Entry expectedEntry{
            .timestamp = Timestamp(1, 2), .size = 110, .count = 4, .hash = boost::none};
        const auto entry = readSizeCount(_collA.uuid);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(expectedEntry, *entry);
    }

    // A second checkpoint does not pick the hash back up either.
    writeCrudEntry(Timestamp(1, 3), _collA, repl::OpTypeEnum::kInsert, 20 /*sizeDelta=*/, kHashB);

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 3), .size = 130, .count = 5, .hash = boost::none};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// Each collection accumulates only its own contributions, and one collection's missing hash
// does not invalidate another's.
TEST_F(SizeCountCheckpointCoordinatorHashTest, HashesTrackedPerCollection) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);
    writeCrudEntry(Timestamp(1, 2), _collB, repl::OpTypeEnum::kInsert, 20 /*sizeDelta=*/, kHashB);
    writeCrudEntry(
        Timestamp(1, 3), _collB, repl::OpTypeEnum::kInsert, 30 /*sizeDelta=*/, boost::none);

    flush();

    const auto entryA = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entryA.has_value());
    EXPECT_EQ((SizeCountStore::Entry{
                  .timestamp = Timestamp(1, 3), .size = 10, .count = 1, .hash = kHashA}),
              *entryA);

    const auto entryB = readSizeCount(_collB.uuid);
    ASSERT_TRUE(entryB.has_value());
    EXPECT_EQ((SizeCountStore::Entry{
                  .timestamp = Timestamp(1, 3), .size = 50, .count = 2, .hash = boost::none}),
              *entryB);
}

// Hashes on the inner operations of an `applyOps` entry are folded the same way as those on
// standalone entries.
TEST_F(SizeCountCheckpointCoordinatorHashTest, HashesOnApplyOpsInnerOpsAreFolded) {
    const Timestamp ts{1, 1};

    BSONArrayBuilder innerOpsBuilder;
    innerOpsBuilder.append(BSON("op" << "i"
                                     << "ns" << _collA.nss.ns_forTest() << "ui" << _collA.uuid
                                     << "o" << BSON("_id" << 1) << "m"
                                     << BSON("sz" << 10 << "h" << kHashA)));
    innerOpsBuilder.append(BSON("op" << "i"
                                     << "ns" << _collA.nss.ns_forTest() << "ui" << _collA.uuid
                                     << "o" << BSON("_id" << 2) << "m"
                                     << BSON("sz" << 20 << "h" << kHashB)));
    innerOpsBuilder.append(BSON("op" << "i"
                                     << "ns" << _collB.nss.ns_forTest() << "ui" << _collB.uuid
                                     << "o" << BSON("_id" << 3) << "m"
                                     << BSON("sz" << 30 << "h" << kHashC)));

    const repl::OplogEntry applyOpsEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(ts, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = BSON("applyOps" << innerOpsBuilder.arr()),
        .wallClockTime = Date_t::now(),
    }};
    writeOplogEntry(applyOpsEntry);

    flush();

    const auto entryA = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entryA.has_value());
    EXPECT_EQ(
        (SizeCountStore::Entry{.timestamp = ts, .size = 30, .count = 2, .hash = kHashA ^ kHashB}),
        *entryA);

    const auto entryB = readSizeCount(_collB.uuid);
    ASSERT_TRUE(entryB.has_value());
    EXPECT_EQ((SizeCountStore::Entry{.timestamp = ts, .size = 30, .count = 1, .hash = kHashC}),
              *entryB);
}

// A collection created and written to within the same checkpoint takes the insert path into
// the store, and the created collection's identity hash folds together with the write's hash.
TEST_F(SizeCountCheckpointCoordinatorHashTest, HashPersistedForCollectionCreatedInCheckpoint) {
    writeOplogEntry(test_helpers::makeCreateOplogEntry(Timestamp(1, 1), _collA));
    writeCrudEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);

    flush();

    const SizeCountStore::Entry expectedEntry{.timestamp = Timestamp(1, 2),
                                              .size = 10,
                                              .count = 1,
                                              .hash = kEmptyCollectionValidationHash ^ kHashA};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// A collection created with no writes persists the hash of an empty collection, which later
// contributions fold into.
TEST_F(SizeCountCheckpointCoordinatorHashTest, CreateWithoutWritesPersistsEmptyCollectionHash) {
    writeOplogEntry(test_helpers::makeCreateOplogEntry(Timestamp(1, 1), _collA));

    flush();
    {
        const SizeCountStore::Entry expectedEntry{.timestamp = Timestamp(1, 1),
                                                  .size = 0,
                                                  .count = 0,
                                                  .hash = kEmptyCollectionValidationHash};
        const auto entry = readSizeCount(_collA.uuid);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(expectedEntry, *entry);
    }

    writeCrudEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 2), .size = 10, .count = 1, .hash = kHashA};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// Dropping a collection removes its persisted entry, hash included, and re-creating the same
// UUID starts a fresh hash rather than resurrecting the dropped one.
TEST_F(SizeCountCheckpointCoordinatorHashTest, DropRemovesPersistedHashAndRecreateStartsFresh) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);

    flush();

    writeOplogEntry(test_helpers::makeDropOplogEntry(Timestamp(1, 2), _collA));

    flush();
    EXPECT_FALSE(readSizeCount(_collA.uuid).has_value());

    writeOplogEntry(test_helpers::makeCreateOplogEntry(Timestamp(1, 3), _collA));
    writeCrudEntry(Timestamp(1, 4), _collA, repl::OpTypeEnum::kInsert, 20 /*sizeDelta=*/, kHashB);

    flush();

    const SizeCountStore::Entry expectedEntry{.timestamp = Timestamp(1, 4),
                                              .size = 20,
                                              .count = 1,
                                              .hash = kEmptyCollectionValidationHash ^ kHashB};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// A `truncateRange` does not carry the hashes of the records it removed, so their
// contributions cannot be folded back out and the persisted hash is invalidated.
TEST_F(SizeCountCheckpointCoordinatorHashTest, TruncateRangeInvalidatesPersistedHash) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 100 /*sizeDelta=*/, kHashA);

    flush();

    test_helpers::writeToOplog(
        _opCtx,
        test_helpers::makeTruncateRangeOplogEntry(
            Timestamp(1, 2), _collA, 40 /*bytesDeleted=*/, 1 /*docsDeleted=*/));

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 2), .size = 60, .count = 0, .hash = boost::none};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// An imported collection's existing documents were never hashed, so the entry it creates
// carries a size and count but no hash.
TEST_F(SizeCountCheckpointCoordinatorHashTest, ImportCollectionPersistsNoHash) {
    test_helpers::writeToOplog(_opCtx,
                               test_helpers::makeImportCollectionOplogEntry(
                                   Timestamp(1, 1), _collA, 3 /*numRecords=*/, 300 /*dataSize=*/));

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 1), .size = 300, .count = 3, .hash = boost::none};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// A prepared transaction's per-operation hashes are dropped at the prepare, and its
// `commitTransaction` entry summarizes only size and count, so the persisted hash is invalidated
// while the size and count still advance.
// TODO SERVER-133315: Carry the transaction's accumulated hash on the commitTransaction entry.
TEST_F(SizeCountCheckpointCoordinatorHashTest, PreparedTxnCommitInvalidatesPersistedHash) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);

    flush();

    const Timestamp prepareTs{1, 2};
    BSONArrayBuilder innerOpsBuilder;
    innerOpsBuilder.append(BSON("op" << "i"
                                     << "ns" << _collA.nss.ns_forTest() << "ui" << _collA.uuid
                                     << "o" << BSON("_id" << 2) << "m"
                                     << BSON("sz" << 50 << "h" << kHashB)));
    const repl::OplogEntry prepareEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(prepareTs, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = BSON("applyOps" << innerOpsBuilder.arr() << "prepare" << true),
        .wallClockTime = Date_t::now(),
        .prevWriteOpTimeInTransaction = repl::OpTime(),
    }};
    writeOplogEntry(prepareEntry);

    MultiOpSizeMetadata commitMetadata;
    commitMetadata.setUuid(_collA.uuid);
    commitMetadata.setSz(50);
    commitMetadata.setCt(1);
    const repl::OplogEntry commitEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(Timestamp(1, 3), 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = BSON("commitTransaction" << 1),
        .sizeMetadata =
            repl::OplogEntrySizeMetadata{std::vector<MultiOpSizeMetadata>{commitMetadata}},
        .wallClockTime = Date_t::now(),
        .prevWriteOpTimeInTransaction = repl::OpTime(prepareTs, 1),
    }};
    writeOplogEntry(commitEntry);

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 3), .size = 60, .count = 2, .hash = boost::none};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// An entry carrying an `h` with no `sz` still contributes its document hash, folded in against a
// zero size and count delta.
// TODO SERVER-133719: Change this to a death test if we decide to invariant upon not having a size
// delta with a hash delta.
TEST_F(SizeCountCheckpointCoordinatorHashTest, HashWithNoSizeDeltaStillFoldsIntoPersistedHash) {
    writeCrudEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/, kHashA);

    SingleOpSizeMetadata hashOnlyMetadata;
    hashOnlyMetadata.setH(kHashB);
    test_helpers::writeToOplog(_opCtx,
                               repl::DurableOplogEntry{repl::DurableOplogEntryParams{
                                   .opTime = repl::OpTime(Timestamp(1, 2), 1),
                                   .opType = repl::OpTypeEnum::kInsert,
                                   .nss = _collA.nss,
                                   .uuid = _collA.uuid,
                                   .oField = BSONObj(),
                                   .sizeMetadata = repl::OplogEntrySizeMetadata{hashOnlyMetadata},
                                   .wallClockTime = Date_t::now(),
                               }});

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 2), .size = 10, .count = 1, .hash = kHashA ^ kHashB};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// The hashes buffered across a chained (unprepared) transaction's applyOps entries become visible
// together when the chain's terminal entry commits, and fold into one persisted hash.
TEST_F(SizeCountCheckpointCoordinatorHashTest, ChainedTxnHashPersistedOnCommit) {
    const Timestamp partialTs{1, 1};
    writeOplogEntry(makeChainedApplyOps(partialTs, kHashA, repl::OpTime()));
    test_helpers::writeToOplog(
        _opCtx,
        makeChainedApplyOps(
            Timestamp(1, 2), kHashB, repl::OpTime(partialTs, 1), /*isPartialTxn=*/false));

    flush();

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 2), .size = 20, .count = 2, .hash = kHashA ^ kHashB};
    const auto entry = readSizeCount(_collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// A chain that never reaches its terminal entry contributes nothing, so no hash is persisted for
// the collection it wrote to.
TEST_F(SizeCountCheckpointCoordinatorHashTest, UnterminatedChainPersistsNoHash) {
    writeOplogEntry(makeChainedApplyOps(Timestamp(1, 1), kHashA, repl::OpTime()));

    flush();

    EXPECT_FALSE(readSizeCount(_collA.uuid).has_value());
}

}  // namespace
}  // namespace mongo::replicated_fast_count

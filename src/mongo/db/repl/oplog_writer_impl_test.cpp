// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_writer_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_batcher_test_fixture.h"
#include "mongo/db/repl/oplog_writer.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/util/fail_point.h"

#include <atomic>

namespace mongo {
namespace repl {
namespace {
using namespace std::literals::string_view_literals;

const auto kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "test"sv);
const auto kNss1 = NamespaceString::createNamespaceString_forTest(kDbName, "foo");
const auto kNss2 = NamespaceString::createNamespaceString_forTest(kDbName, "bar");

BSONObj makeRawInsertOplogEntry(int t, const NamespaceString& nss) {
    auto entry = makeInsertOplogEntry(t, nss);
    return entry.getEntry().getRaw();
}

// Insert entry with a payload of about 'payloadBytes' bytes.
BSONObj makeLargeRawInsertOplogEntry(int t, const NamespaceString& nss, size_t payloadBytes) {
    std::string payload(payloadBytes, 'x');
    BSONObj oField = BSON("_id" << t << "a" << t << "payload" << payload);
    DurableOplogEntry durable(OpTime(Timestamp(t, 1), 1),       // optime
                              OpTypeEnum::kInsert,              // op type
                              nss,                              // namespace
                              boost::none,                      // uuid
                              boost::none,                      // fromMigrate
                              boost::none,                      // checkExistenceForDiffInsert
                              boost::none,                      // versionContext
                              repl::OplogEntry::kOplogVersion,  // version
                              oField,                           // o
                              boost::none,                      // o2
                              {},                               // sessionInfo
                              boost::none,                      // upsert
                              Date_t() + Seconds(t),            // wall clock time
                              {},                               // statement ids
                              boost::none,  // optime of previous write within same transaction
                              boost::none,  // pre-image optime
                              boost::none,  // post-image optime
                              boost::none,  // ShardId of resharding recipient
                              boost::none,  // _id
                              boost::none)  // needsRetryImage
        ;
    return OplogEntry(durable).getEntry().getRaw();
}


class JournalListenerMock : public JournalListener {
public:
    class Token : public JournalListener::Token {
    public:
        Token(OpTimeAndWallTime opTimeAndWallTime, bool isPrimary)
            : opTimeAndWallTime(opTimeAndWallTime), isPrimary(isPrimary) {}
        OpTimeAndWallTime opTimeAndWallTime;
        bool isPrimary;
    };

    std::unique_ptr<JournalListener::Token> getToken(OperationContext* opCtx,
                                                     TokenMode /*mode*/) override {
        return std::make_unique<Token>(
            repl::ReplicationCoordinator::get(opCtx)->getMyLastWrittenOpTimeAndWallTime(true),
            false /* isPrimary */);
    }

    void onDurable(const JournalListener::Token& t) override {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& token = dynamic_cast<const Token&>(t);
        _onDurableToken = token.opTimeAndWallTime;
    }

    OpTimeAndWallTime getOnDurableToken() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _onDurableToken;
    }

private:
    std::mutex _mutex;
    OpTimeAndWallTime _onDurableToken;
};

class CountingConsistencyMarkers : public ReplicationConsistencyMarkersMock {
public:
    void setOplogTruncateAfterPoint(OperationContext* opCtx, const Timestamp& timestamp) override {
        ReplicationConsistencyMarkersMock::setOplogTruncateAfterPoint(opCtx, timestamp);
        _truncatePointUpdates.fetch_add(1);
    }

    size_t truncatePointUpdates() const {
        return _truncatePointUpdates.load();
    }

private:
    std::atomic<size_t> _truncatePointUpdates{0};
};

class OplogWriterImplTest : public ServiceContextMongoDTest {
protected:
    explicit OplogWriterImplTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true).useJournalListener(
              std::make_unique<JournalListenerMock>())) {}

    void setUp() override;
    void tearDown() override;

    OperationContext* opCtx() const;

    ThreadPool* getWriterPool() const;
    ReplicationCoordinator* getReplCoord() const;
    StorageInterface* getStorageInterface() const;
    ReplicationConsistencyMarkers* getConsistencyMarkers() const;
    JournalListenerMock* getJournalListener() const;
    int getOplogDocsCount() const;

    template <typename T>
    void assertScheduledRanges(const std::vector<T>& ops,
                               size_t expectedRanges,
                               size_t maxThreads = 4) {
        auto poolOptions = getWriterPool()->getStats().options;
        poolOptions.poolName = "OplogWriterSplitTest";
        poolOptions.threadNamePrefix = "OplogWriterSplitTest-";
        poolOptions.minThreads = 0;
        poolOptions.maxThreads = maxThreads;
        auto pool = ThreadPool::make(std::move(poolOptions));
        OplogWriterImpl writer(nullptr,
                               nullptr,
                               nullptr,
                               pool.get(),
                               getReplCoord(),
                               getStorageInterface(),
                               getConsistencyMarkers(),
                               OplogWriter::Options(false));
        ON_BLOCK_EXIT([&] {
            pool->shutdown();
            pool->join();
        });

        const OpTime lastWritten(Timestamp(1, 1), 1);
        getReplCoord()->setMyLastWrittenOpTimeAndWallTimeForward({lastWritten, Date_t::now()});
        ASSERT_EQ(Timestamp(), getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx()));
        const auto initialUpdates = _consistencyMarkers->truncatePointUpdates();
        auto* fp = globalFailPointRegistry().find("hangOplogWriterBeforeWritingOplogBatchRange");
        ASSERT(fp);
        ON_BLOCK_EXIT([&] { fp->setMode(FailPoint::off); });
        // A one-shot hit disables the fail point before pauseWhileSet(), so even a regression
        // that exposes synchronous writes to the hook will fail an assertion rather than hang.
        const auto initialTimesEntered =
            fp->setMode(FailPoint::nTimes, 1, BSON("beginOffset" << 0));
        if constexpr (std::is_same_v<T, BSONObj>) {
            using Result = OplogWriterImpl::ScheduleWriteResult;
            ASSERT_EQ(expectedRanges == 0 ? Result::kWritten : Result::kScheduled,
                      writer.scheduleWriteOplogBatch(opCtx(), ops));
        } else {
            ASSERT(writer.scheduleWriteOplogBatch(opCtx(), ops));
        }

        // An unstarted pool keeps all asynchronous ranges queued.
        ASSERT_EQ(expectedRanges, pool->getStats().numPendingTasks);
        ASSERT_EQ(0, pool->getStats().numThreads);
        if (expectedRanges == 0) {
            ASSERT_EQ(ops.size(), static_cast<size_t>(getOplogDocsCount()));
            ASSERT_EQ(initialUpdates, _consistencyMarkers->truncatePointUpdates());
        } else {
            ASSERT_EQ(0, getOplogDocsCount());
            ASSERT_EQ(lastWritten.getTimestamp(),
                      getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx()));
            ASSERT_EQ(initialUpdates + 1, _consistencyMarkers->truncatePointUpdates());
            pool->startup();
            writer.waitForScheduledWrites(opCtx());
            ASSERT_EQ(initialUpdates + 2, _consistencyMarkers->truncatePointUpdates());
        }
        ASSERT_EQ(ops.size(), static_cast<size_t>(getOplogDocsCount()));
        ASSERT_EQ(Timestamp(), getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx()));
        ASSERT_EQ(lastWritten, getReplCoord()->getMyLastWrittenOpTime());
        const auto expectedHits = std::is_same_v<T, BSONObj> && expectedRanges > 0 ? 1 : 0;
        ASSERT_EQ(initialTimesEntered + expectedHits, fp->setMode(FailPoint::off));
    }

    ServiceContext* _serviceContext;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    std::unique_ptr<ThreadPool> _workerPool;
    std::unique_ptr<CountingConsistencyMarkers> _consistencyMarkers;
};

void OplogWriterImplTest::setUp() {
    ServiceContextMongoDTest::setUp();

    _serviceContext = getServiceContext();
    _opCtxHolder = makeOperationContext();

    ReplicationCoordinator::set(_serviceContext,
                                std::make_unique<ReplicationCoordinatorMock>(_serviceContext));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    StorageInterface::set(_serviceContext, std::make_unique<StorageInterfaceImpl>());

    _consistencyMarkers = std::make_unique<CountingConsistencyMarkers>();

    MongoDSessionCatalog::set(
        _serviceContext,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

    repl::createOplog(opCtx());

    _workerPool = makeReplWorkerPool();
}

int OplogWriterImplTest::getOplogDocsCount() const {
    AutoGetOplogFastPath oplogRead(opCtx(), OplogAccessMode::kRead);
    const auto& oplog = oplogRead.getCollection();
    return oplog->getRecordStore()->numRecords();
}

void OplogWriterImplTest::tearDown() {
    _opCtxHolder = {};
    _workerPool = {};
    _consistencyMarkers = {};
    StorageInterface::set(_serviceContext, {});
    ServiceContextMongoDTest::tearDown();
}

OperationContext* OplogWriterImplTest::opCtx() const {
    return _opCtxHolder.get();
}

ThreadPool* OplogWriterImplTest::getWriterPool() const {
    return _workerPool.get();
}

ReplicationCoordinator* OplogWriterImplTest::getReplCoord() const {
    return ReplicationCoordinator::get(_serviceContext);
}

StorageInterface* OplogWriterImplTest::getStorageInterface() const {
    return StorageInterface::get(_serviceContext);
}

ReplicationConsistencyMarkers* OplogWriterImplTest::getConsistencyMarkers() const {
    return _consistencyMarkers.get();
}

JournalListenerMock* OplogWriterImplTest::getJournalListener() const {
    return static_cast<JournalListenerMock*>(journalListener());
}

TEST_F(OplogWriterImplTest, DedicatedWriterPoolCreatesAllResidentWorkers) {
    unittest::ServerParameterGuard applierMinThreads("replWriterMinThreadCount", 1);
    for (size_t threadCount : {2U, 4U}) {
        std::atomic<size_t> completed{0};
        auto pool = makeOplogWriterWorkerPool(threadCount);
        ON_BLOCK_EXIT([&] {
            pool->shutdown();
            pool->join();
        });
        const auto stats = pool->getStats();
        ASSERT_EQ(threadCount, stats.options.minThreads);
        ASSERT_EQ(threadCount, stats.options.maxThreads);
        ASSERT_EQ(threadCount, stats.numThreads);
        ASSERT_EQ(0U, stats.numPendingTasks);
        for (size_t i = 0; i < threadCount; ++i) {
            pool->schedule([&](Status status) {
                invariant(status);
                completed.fetch_add(1);
            });
        }
        pool->waitForIdle();
        ASSERT_EQ(threadCount, completed.load());
        ASSERT_EQ(threadCount, pool->getStats().numThreads);
    }
}

TEST_F(OplogWriterImplTest, DedicatedWriterPoolRejectsIncompleteStartupAndCleansUp) {
    auto countWriterClients = [&] {
        size_t count = 0;
        for (auto cursor = getServiceContext()->makeClientsCursor(); cursor; cursor.next()) {
            if (cursor.value()->desc().starts_with("OplogWriterWorker-")) {
                ++count;
            }
        }
        return count;
    };
    const auto initialClients = countWriterClients();
    // Starts fewer workers than requested.
    for (int startupThreads : {0, 1}) {
        {
            FailPointEnableBlock fp("overrideOplogWriterPoolStartupThreadCount",
                                    BSON("startupThreads" << startupThreads));
            ASSERT_THROWS_CODE(
                makeOplogWriterWorkerPool(4), DBException, ErrorCodes::InternalError);
        }
        ASSERT_EQ(initialClients, countWriterClients());

        auto pool = makeOplogWriterWorkerPool(4);
        ASSERT_EQ(4U, pool->getStats().numThreads);
        pool->shutdown();
        pool->join();
        ASSERT_EQ(initialClients, countWriterClients());
    }
}

using OplogWriterImplTestDeathTest = OplogWriterImplTest;
DEATH_TEST_F(OplogWriterImplTestDeathTest,
             DedicatedWriterPoolRejectsZeroThreadCount,
             "threadCount >= 2") {
    makeOplogWriterWorkerPool(0);
}

DEATH_TEST_F(OplogWriterImplTestDeathTest,
             DedicatedWriterPoolRejectsSingleThreadCount,
             "threadCount >= 2") {
    makeOplogWriterWorkerPool(1);
}

DEATH_TEST_F(OplogWriterImplTestDeathTest, WriteEmptyBatchFails, "!ops.empty()") {
    OplogWriter::Options options(false /* skipWritesToOplogColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                options);

    // Writing an empty batch should hit an invariant.
    oplogWriter.writeOplogBatch(opCtx(), std::vector<BSONObj>{});
}

TEST_F(OplogWriterImplTest, WriteOplogCollection) {
    OplogWriter::Options options(false /* skipWritesToOplogColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                options);

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    auto written = oplogWriter.writeOplogBatch(opCtx(), std::move(ops));

    // Verify that the batch is only written to the oplog collection.
    ASSERT(written);
    ASSERT_EQ(2, getOplogDocsCount());
}

TEST_F(OplogWriterImplTest, SkipWriteToOplogCollection) {
    OplogWriter::Options options(true /* skipWritesToOplogColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                options);

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    auto written = oplogWriter.writeOplogBatch(opCtx(), std::move(ops));

    // Verify that the batch is not written any collection.
    ASSERT(!written);
    ASSERT_EQ(0, getOplogDocsCount());
}

TEST_F(OplogWriterImplTest, finalizeOplogBatchCorrectlyUpdatesOpTimes) {
    OplogWriter::Options options(false /* skipWritesToOplogColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                options);

    auto curOpTime = OpTime(Timestamp(2, 2), 1);
    auto curWallTime = Date_t::now();
    OpTimeAndWallTime curOpTimeAndWallTime{curOpTime, curWallTime};

    getReplCoord()->setMyLastWrittenOpTimeAndWallTimeForward(curOpTimeAndWallTime);
    getReplCoord()->setMyLastAppliedOpTimeAndWallTimeForward(curOpTimeAndWallTime);
    getReplCoord()->setMyLastDurableOpTimeAndWallTimeForward(curOpTimeAndWallTime);

    OpTime newOpTime(curOpTime.getTimestamp() + 16, curOpTime.getTerm());
    Date_t newWallTime = curWallTime + Seconds(10);
    OpTimeAndWallTime newOpTimeAndWallTime{newOpTime, newWallTime};

    oplogWriter.finalizeOplogBatch(opCtx(), newOpTimeAndWallTime, true);

    // The finalizeOplogBatch() function only triggers the journal flusher but does not
    // wait for it, so we sleep for a while and verify that the lastWritten opTime has
    // been correctly updated and that the journal flusher has finished and invoked the
    // onDurable callback. The test fixture disables periodic journal flush by default,
    // making sure that it is finalizeOplogBatch() that triggers the journal flush.
    sleepmillis(2000);
    ASSERT_EQ(newOpTimeAndWallTime, getReplCoord()->getMyLastWrittenOpTimeAndWallTime());
    ASSERT_EQ(newOpTimeAndWallTime, getJournalListener()->getOnDurableToken());
}

std::vector<BSONObj> makeOplogBatchWithTotalBytes(size_t numOps, size_t totalBytes) {
    invariant(numOps > 0);
    std::vector<BSONObj> ops;
    ops.reserve(numOps);
    size_t remainingBytes = totalBytes;
    for (size_t i = 0; i < numOps; ++i) {
        const auto t = static_cast<int>(i + 2);
        const auto overhead =
            static_cast<size_t>(makeLargeRawInsertOplogEntry(t, kNss1, 0).objsize());
        const size_t entryBytes = remainingBytes / (numOps - i);
        ASSERT_GTE(entryBytes, overhead);
        auto op = makeLargeRawInsertOplogEntry(t, kNss1, entryBytes - overhead);
        ASSERT_EQ(entryBytes, static_cast<size_t>(op.objsize()));
        ops.push_back(std::move(op));
        remainingBytes -= entryBytes;
    }
    ASSERT_EQ(0U, remainingBytes);
    return ops;
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchBytesScheduleThreeRanges) {
    assertScheduledRanges(makeOplogBatchWithTotalBytes(12, 6 * 1024 * 1024), 3);
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchManySmallEntriesWriteInline) {
    std::vector<BSONObj> ops;
    for (int t = 2; t < 5002; ++t) {
        ops.push_back(makeRawInsertOplogEntry(t, kNss1));
    }
    assertScheduledRanges(ops, 0);
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchLargeDocumentsWritesAllOps) {
    std::vector<BSONObj> ops;
    for (int t = 2; t < 10; ++t) {
        ops.push_back(makeLargeRawInsertOplogEntry(t, kNss1, 2 * 1024 * 1024));
    }
    assertScheduledRanges(ops, 4);
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchSingleOpWritesInline) {
    assertScheduledRanges(std::vector<BSONObj>{makeRawInsertOplogEntry(2, kNss1)}, 0);
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchSmallBatchWritesInline) {
    std::vector<BSONObj> ops;
    for (int t = 2; t < 33; ++t) {
        ops.push_back(makeRawInsertOplogEntry(t, kNss1));
    }
    assertScheduledRanges(ops, 0);
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchBelowByteThresholdWritesInline) {
    assertScheduledRanges(makeOplogBatchWithTotalBytes(4, 4 * 1024 * 1024 - 1), 0);
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchAtByteThresholdSchedulesTwoRanges) {
    assertScheduledRanges(makeOplogBatchWithTotalBytes(4, 4 * 1024 * 1024), 2);
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchLargeSingleRangeWritesInline) {
    std::vector<BSONObj> ops;
    for (int t = 2; t < 6; ++t) {
        ops.push_back(makeLargeRawInsertOplogEntry(t, kNss1, 512 * 1024));
    }
    assertScheduledRanges(ops, 0);
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchSingleLargeEntryWritesInline) {
    assertScheduledRanges(makeOplogBatchWithTotalBytes(1, 8 * 1024 * 1024), 0);
}

TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchByteRangeCountIsCappedByEntryCount) {
    assertScheduledRanges(makeOplogBatchWithTotalBytes(2, 8 * 1024 * 1024), 2);
}

// Asserts the ranges are exactly 'numRanges' contiguous, non-empty ranges covering 'ops'.
std::vector<std::pair<size_t, size_t>> splitAndCheckRanges(const std::vector<BSONObj>& ops,
                                                           size_t numRanges) {
    size_t totalBytes = 0;
    for (const auto& op : ops) {
        totalBytes += static_cast<size_t>(op.objsize());
    }
    auto ranges = OplogWriterImpl::splitOplogBatchIntoRanges(ops, totalBytes, numRanges);
    ASSERT_EQ(numRanges, ranges.size());
    size_t expectedBegin = 0;
    for (const auto& [begin, end] : ranges) {
        ASSERT_EQ(expectedBegin, begin);
        ASSERT_LT(begin, end);
        expectedBegin = end;
    }
    ASSERT_EQ(ops.size(), expectedBegin);
    return ranges;
}

TEST_F(OplogWriterImplTest, SplitRangesAreContiguousForAllRangeCounts) {
    std::vector<BSONObj> ops;
    for (int t = 2; t < 42; ++t) {
        // Mix small and large entries in an irregular pattern.
        ops.push_back(t % 7 == 0 ? makeLargeRawInsertOplogEntry(t, kNss1, 64 * 1024)
                                 : makeRawInsertOplogEntry(t, kNss1));
    }
    for (size_t numOps = 2; numOps <= ops.size(); ++numOps) {
        std::vector<BSONObj> prefix(ops.begin(), ops.begin() + numOps);
        for (size_t numRanges = 1; numRanges <= numOps; ++numRanges) {
            splitAndCheckRanges(prefix, numRanges);
        }
    }
}

// One large entry followed by many small ones. Splitting by bytes alone would put almost every
// small entry in one range; the entry-count limit must spread them out.
TEST_F(OplogWriterImplTest, SplitLargeEntryFirstBalancesSmallEntries) {
    std::vector<BSONObj> ops;
    ops.push_back(makeLargeRawInsertOplogEntry(2, kNss1, 8 * 1024 * 1024));
    for (int t = 3; t < 5002; ++t) {
        ops.push_back(makeRawInsertOplogEntry(t, kNss1));
    }
    const auto ranges = splitAndCheckRanges(ops, 4);
    ASSERT_EQ(std::make_pair(size_t{0}, size_t{1}), ranges[0]);
    for (size_t r = 1; r < ranges.size(); ++r) {
        const auto numOps = ranges[r].second - ranges[r].first;
        ASSERT_GTE(numOps, 1600U);
        ASSERT_LTE(numOps, 1700U);
    }
}

// Many small entries followed by one large one. No range may hold much more than its share of
// entries, and the large entry ends up in the last range.
TEST_F(OplogWriterImplTest, SplitLargeEntryLastCapsEntriesPerRange) {
    std::vector<BSONObj> ops;
    for (int t = 2; t < 5001; ++t) {
        ops.push_back(makeRawInsertOplogEntry(t, kNss1));
    }
    ops.push_back(makeLargeRawInsertOplogEntry(5001, kNss1, 8 * 1024 * 1024));
    const auto ranges = splitAndCheckRanges(ops, 4);
    for (const auto& [begin, end] : ranges) {
        ASSERT_LTE(end - begin, 1250U);
    }
}

// Equally sized entries split into equal ranges.
TEST_F(OplogWriterImplTest, SplitUniformEntriesEvenly) {
    std::vector<BSONObj> ops;
    for (int t = 2; t < 10; ++t) {
        ops.push_back(makeLargeRawInsertOplogEntry(t, kNss1, 1024 * 1024));
    }
    const auto ranges = splitAndCheckRanges(ops, 4);
    for (const auto& [begin, end] : ranges) {
        ASSERT_EQ(2U, end - begin);
    }
}

TEST_F(OplogWriterImplTest, OplogEntrySmallBatchKeepsSingleRange) {
    std::vector<OplogEntry> ops;
    for (int t = 2; t < 52; ++t) {
        ops.push_back(makeInsertOplogEntry(t, kNss1));
    }
    assertScheduledRanges(ops, 1);
}

TEST_F(OplogWriterImplTest, OplogEntryAtThreadThresholdUsesAllRanges) {
    std::vector<OplogEntry> ops;
    for (int t = 2; t < 66; ++t) {
        ops.push_back(makeInsertOplogEntry(t, kNss1));
    }
    assertScheduledRanges(ops, 4);
}

TEST_F(OplogWriterImplTest, OplogEntryLargeDocumentsKeepSingleRange) {
    std::vector<OplogEntry> ops;
    for (int t = 2; t < 10; ++t) {
        ops.emplace_back(makeLargeRawInsertOplogEntry(t, kNss1, 1024 * 1024));
    }
    assertScheduledRanges(ops, 1);
}

// An unfinished batch keeps its truncate point until the range writes complete.
TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchManagesTruncateAfterPoint) {
    OplogWriter::Options options(false /* skipWritesToOplogColl */);

    OplogWriterImpl oplogWriter(nullptr,          // executor
                                nullptr,          // writeBuffer
                                nullptr,          // applyBuffer
                                getWriterPool(),  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                options);

    // Simulate an already-written oplog position so the truncate point is set to a non-null
    // timestamp during the write.
    auto lastWritten = OpTime(Timestamp(2, 2), 1);
    getReplCoord()->setMyLastWrittenOpTimeAndWallTimeForward({lastWritten, Date_t::now()});

    // Before writing, the truncate point should be null.
    ASSERT_EQ(Timestamp(), getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx()));

    std::vector<BSONObj> ops;
    for (int t = 3; t < 11; ++t) {
        ops.push_back(makeLargeRawInsertOplogEntry(t, kNss1, 1024 * 1024));
    }

    auto* fp = globalFailPointRegistry().find("hangOplogWriterBeforeWritingOplogBatchRange");
    ASSERT(fp);
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([&] {
        fp->setMode(FailPoint::off);
        getWriterPool()->waitForIdle();
    });

    ASSERT_EQ(OplogWriterImpl::ScheduleWriteResult::kScheduled,
              oplogWriter.scheduleWriteOplogBatch(opCtx(), ops));
    // Each held range enters the fail point twice.
    opCtx()->runWithDeadline(Date_t::now() + Seconds(30), ErrorCodes::ExceededTimeLimit, [&] {
        fp->waitForTimesEntered(opCtx(), initialTimesEntered + 2);
    });
    ASSERT_EQ(0, getOplogDocsCount());
    ASSERT_EQ(lastWritten.getTimestamp(),
              getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx()));
    ASSERT_EQ(lastWritten, getReplCoord()->getMyLastWrittenOpTime());

    fp->setMode(FailPoint::off);
    oplogWriter.waitForScheduledWrites(opCtx());
    ASSERT_EQ(Timestamp(), getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx()));
    ASSERT_EQ(ops.size(), static_cast<size_t>(getOplogDocsCount()));
}

// Verifies that on the worker-pool path, skipWritesToOplogColl still results in no writes and
// no scheduling, mirroring the single-threaded behavior.
TEST_F(OplogWriterImplTest, ScheduleWriteOplogBatchSkipWritesWritesNothing) {
    OplogWriter::Options options(true /* skipWritesToOplogColl */);

    OplogWriterImpl oplogWriter(nullptr,          // executor
                                nullptr,          // writeBuffer
                                nullptr,          // applyBuffer
                                getWriterPool(),  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                options);

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    const Timestamp truncatePoint(1, 1);
    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx(), truncatePoint);
    const auto initialUpdates = _consistencyMarkers->truncatePointUpdates();
    ASSERT_EQ(OplogWriterImpl::ScheduleWriteResult::kSkipped,
              oplogWriter.scheduleWriteOplogBatch(opCtx(), ops));

    ASSERT_EQ(0, getOplogDocsCount());
    ASSERT_EQ(initialUpdates, _consistencyMarkers->truncatePointUpdates());
    ASSERT_EQ(truncatePoint, getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx()));
}

// Same invariant as writeOplogBatch().
DEATH_TEST_F(OplogWriterImplTestDeathTest, ScheduleWriteEmptyBatchFails, "!ops.empty()") {
    OplogWriter::Options options(false /* skipWritesToOplogColl */);

    OplogWriterImpl oplogWriter(nullptr,          // executor
                                nullptr,          // writeBuffer
                                nullptr,          // applyBuffer
                                getWriterPool(),  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                options);

    oplogWriter.scheduleWriteOplogBatch(opCtx(), std::vector<BSONObj>{});
}

}  // namespace
}  // namespace repl
}  // namespace mongo

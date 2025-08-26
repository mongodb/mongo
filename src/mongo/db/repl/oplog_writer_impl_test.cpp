/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_writer_impl.h"

#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/op_observer/op_observer_registry.h"
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
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace repl {
namespace {

const auto kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "test"_sd);
const auto kNss1 = NamespaceString::createNamespaceString_forTest(kDbName, "foo");
const auto kNss2 = NamespaceString::createNamespaceString_forTest(kDbName, "bar");

BSONObj makeRawInsertOplogEntry(int t, const NamespaceString& nss) {
    auto entry = makeInsertOplogEntry(t, nss);
    return entry.getEntry().getRaw();
}

class CountOpsObserver : public OplogWriterImpl::Observer {
public:
    void onWriteOplogCollection(std::vector<InsertStatement>::const_iterator begin,
                                std::vector<InsertStatement>::const_iterator end) final {
        oplogCollDocsCount.addAndFetch(std::distance(begin, end));
    }

    void onWriteChangeCollections(std::vector<InsertStatement>::const_iterator begin,
                                  std::vector<InsertStatement>::const_iterator end) final {
        changeCollDocsCount.addAndFetch(std::distance(begin, end));
    }

    AtomicWord<int> oplogCollDocsCount{0};
    AtomicWord<int> changeCollDocsCount{0};
};

class JournalListenerMock : public JournalListener {
public:
    class Token : public JournalListener::Token {
    public:
        Token(OpTimeAndWallTime opTimeAndWallTime, bool isPrimary)
            : opTimeAndWallTime(opTimeAndWallTime), isPrimary(isPrimary) {}
        OpTimeAndWallTime opTimeAndWallTime;
        bool isPrimary;
    };

    std::unique_ptr<JournalListener::Token> getToken(OperationContext* opCtx) override {
        return std::make_unique<Token>(
            repl::ReplicationCoordinator::get(opCtx)->getMyLastWrittenOpTimeAndWallTime(true),
            false /* isPrimary */);
    }

    void onDurable(const JournalListener::Token& t) override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        auto& token = dynamic_cast<const Token&>(t);
        _onDurableToken = token.opTimeAndWallTime;
    }

    OpTimeAndWallTime getOnDurableToken() {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _onDurableToken;
    }

private:
    stdx::mutex _mutex;
    OpTimeAndWallTime _onDurableToken;
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

    ServiceContext* _serviceContext;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    std::unique_ptr<ThreadPool> _workerPool;
    std::unique_ptr<ReplicationConsistencyMarkers> _consistencyMarkers;
    std::unique_ptr<CountOpsObserver> _observer;
};

void OplogWriterImplTest::setUp() {
    ServiceContextMongoDTest::setUp();

    _serviceContext = getServiceContext();
    _opCtxHolder = makeOperationContext();

    ReplicationCoordinator::set(_serviceContext,
                                std::make_unique<ReplicationCoordinatorMock>(_serviceContext));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    StorageInterface::set(_serviceContext, std::make_unique<StorageInterfaceImpl>());

    _consistencyMarkers = std::make_unique<ReplicationConsistencyMarkersMock>();

    MongoDSessionCatalog::set(
        _serviceContext,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

    repl::createOplog(opCtx());

    _workerPool = makeReplWorkerPool();
    _observer = std::make_unique<CountOpsObserver>();
}

void OplogWriterImplTest::tearDown() {
    _opCtxHolder = {};
    _workerPool = {};
    _consistencyMarkers = {};
    _observer = {};
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

DEATH_TEST_F(OplogWriterImplTest, WriteEmptyBatchFails, "!ops.empty()") {
    OplogWriter::Options options(false /* skipWritesToOplogColl */,
                                 false /* skipWritesToChangeColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                &noopOplogWriterObserver,
                                options);

    // Writing an empty batch should hit an invariant.
    oplogWriter.writeOplogBatch(opCtx(), std::vector<BSONObj>{});
}

TEST_F(OplogWriterImplTest, WriteOplogCollectionOnly) {
    OplogWriter::Options options(false /* skipWritesToOplogColl */,
                                 true /* skipWritesToChangeColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                _observer.get(),
                                options);

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    auto written = oplogWriter.writeOplogBatch(opCtx(), std::move(ops));

    // Verify that the batch is only written to the oplog collection.
    ASSERT(written);
    ASSERT_EQ(2, _observer->oplogCollDocsCount.load());
    ASSERT_EQ(0, _observer->changeCollDocsCount.load());
}

TEST_F(OplogWriterImplTest, WriteChangeCollectionsOnly) {
    RAIIServerParameterControllerForTest changeStreamFeatureFlagController{
        "featureFlagServerlessChangeStreams", true};
    RAIIServerParameterControllerForTest changeStreamTestFlagController{
        "internalChangeStreamUseTenantIdForTesting", true};

    ChangeStreamChangeCollectionManager::create(_serviceContext);

    OplogWriter::Options options(true /* skipWritesToOplogColl */,
                                 false /* skipWritesToChangeColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                _observer.get(),
                                options);

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    auto written = oplogWriter.writeOplogBatch(opCtx(), std::move(ops));

    // Verify that the batch is only written to the change collections.
    ASSERT(written);
    ASSERT_EQ(0, _observer->oplogCollDocsCount.load());
    ASSERT_EQ(2, _observer->changeCollDocsCount.load());
}

TEST_F(OplogWriterImplTest, WriteBothOplogAndChangeCollections) {
    RAIIServerParameterControllerForTest changeStreamFeatureFlagController{
        "featureFlagServerlessChangeStreams", true};
    RAIIServerParameterControllerForTest changeStreamTestFlagController{
        "internalChangeStreamUseTenantIdForTesting", true};

    ChangeStreamChangeCollectionManager::create(_serviceContext);

    OplogWriter::Options options(false /* skipWritesToOplogColl */,
                                 false /* skipWritesToChangeColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                _observer.get(),
                                options);

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    auto written = oplogWriter.writeOplogBatch(opCtx(), std::move(ops));

    // Verify that the batch written to both the oplog and change collections.
    ASSERT(written);
    ASSERT_EQ(2, _observer->oplogCollDocsCount.load());
    ASSERT_EQ(2, _observer->changeCollDocsCount.load());
}

TEST_F(OplogWriterImplTest, WriteNeitherOplogNorChangeCollections) {
    OplogWriter::Options options(true /* skipWritesToOplogColl */,
                                 true /* skipWritesToChangeColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                _observer.get(),
                                options);

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    auto written = oplogWriter.writeOplogBatch(opCtx(), std::move(ops));

    // Verify that the batch is not written any collection.
    ASSERT(!written);
    ASSERT_EQ(0, _observer->oplogCollDocsCount.load());
    ASSERT_EQ(0, _observer->changeCollDocsCount.load());
}

TEST_F(OplogWriterImplTest, finalizeOplogBatchCorrectlyUpdatesOpTimes) {
    OplogWriter::Options options(false /* skipWritesToOplogColl */,
                                 false /* skipWritesToChangeColl */);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                nullptr,  // workerPool
                                getReplCoord(),
                                getStorageInterface(),
                                getConsistencyMarkers(),
                                &noopOplogWriterObserver,
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

}  // namespace
}  // namespace repl
}  // namespace mongo

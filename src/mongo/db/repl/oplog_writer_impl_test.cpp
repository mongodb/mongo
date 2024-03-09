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

#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_batcher_test_fixture.h"
#include "mongo/db/repl/oplog_writer.h"
#include "mongo/db/repl/oplog_writer_impl.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/idl/server_parameter_test_util.h"
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
    void onWriteOplogCollection(const std::vector<InsertStatement>& docs) final {
        oplogCollDocsCount += docs.size();
    }

    void onWriteChangeCollection(const std::vector<InsertStatement>& docs) final {
        changeCollDocsCount += docs.size();
    }

    int oplogCollDocsCount = 0;
    int changeCollDocsCount = 0;
};

class JournalListenerMock : public JournalListener {
public:
    Token getToken(OperationContext* opCtx) {
        return {repl::ReplicationCoordinator::get(opCtx)->getMyLastWrittenOpTimeAndWallTime(true),
                false /* isPrimary */};
    }

    void onDurable(const Token& token) {
        stdx::lock_guard<Latch> lock(_mutex);
        _onDurableToken = token.first;
    }

    OpTimeAndWallTime getOnDurableToken() {
        stdx::lock_guard<Latch> lock(_mutex);
        return _onDurableToken;
    }

private:
    Mutex _mutex = MONGO_MAKE_LATCH("JournalListenerMock::_mutex");
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

    ReplicationCoordinator* getReplCoord() const;
    StorageInterface* getStorageInterface() const;
    JournalListenerMock* getJournalListener() const;

    ServiceContext* _serviceContext;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    std::unique_ptr<ThreadPool> _writerPool;
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

    MongoDSessionCatalog::set(
        _serviceContext,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

    repl::createOplog(opCtx());

    _writerPool = makeReplWriterPool();
    _observer = std::make_unique<CountOpsObserver>();
}

void OplogWriterImplTest::tearDown() {
    _opCtxHolder = {};
    _writerPool = {};
    _observer = {};
    StorageInterface::set(_serviceContext, {});
    ServiceContextMongoDTest::tearDown();
}

OperationContext* OplogWriterImplTest::opCtx() const {
    return _opCtxHolder.get();
}

ReplicationCoordinator* OplogWriterImplTest::getReplCoord() const {
    return ReplicationCoordinator::get(_serviceContext);
}

StorageInterface* OplogWriterImplTest::getStorageInterface() const {
    return StorageInterface::get(_serviceContext);
}

JournalListenerMock* OplogWriterImplTest::getJournalListener() const {
    return static_cast<JournalListenerMock*>(_journalListener.get());
}

DEATH_TEST_F(OplogWriterImplTest, WriteEmptyBatchFails, "!ops.empty()") {
    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                getReplCoord(),
                                getStorageInterface(),
                                _writerPool.get(),
                                &noopOplogWriterObserver,
                                OplogWriter::Options());

    // Writing an empty batch should hit an invariant.
    oplogWriter.writeOplogBatch(opCtx(), {}).getStatus().ignore();
}

TEST_F(OplogWriterImplTest, WriteOplogCollectionOnly) {
    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                getReplCoord(),
                                getStorageInterface(),
                                _writerPool.get(),
                                _observer.get(),
                                OplogWriter::Options());

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    auto returnOpTime = OpTime::parseFromOplogEntry(ops.back()).getValue();
    auto statusWith = oplogWriter.writeOplogBatch(opCtx(), std::move(ops));

    ASSERT_OK(statusWith);
    ASSERT_EQ(returnOpTime, statusWith.getValue());

    // Verify that the batch is only written to the oplog collection.
    ASSERT_EQ(2, _observer->oplogCollDocsCount);
    ASSERT_EQ(0, _observer->changeCollDocsCount);
}

TEST_F(OplogWriterImplTest, WriteChangeCollectionOnly) {
    RAIIServerParameterControllerForTest changeStreamFeatureFlagController{
        "featureFlagServerlessChangeStreams", true};
    RAIIServerParameterControllerForTest changeStreamTestFlagController{
        "internalChangeStreamUseTenantIdForTesting", true};

    ChangeStreamChangeCollectionManager::create(_serviceContext);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                getReplCoord(),
                                getStorageInterface(),
                                _writerPool.get(),
                                _observer.get(),
                                OplogWriter::Options(true /* skipWritesToOplogColl */));

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    auto returnOpTime = OpTime::parseFromOplogEntry(ops.back()).getValue();
    auto statusWith = oplogWriter.writeOplogBatch(opCtx(), std::move(ops));

    ASSERT_OK(statusWith);
    ASSERT_EQ(returnOpTime, statusWith.getValue());

    // Verify that the batch is only written to the change collection.
    ASSERT_EQ(0, _observer->oplogCollDocsCount);
    ASSERT_EQ(2, _observer->changeCollDocsCount);
}

TEST_F(OplogWriterImplTest, WriteBothOplogAndChangeCollection) {
    RAIIServerParameterControllerForTest changeStreamFeatureFlagController{
        "featureFlagServerlessChangeStreams", true};
    RAIIServerParameterControllerForTest changeStreamTestFlagController{
        "internalChangeStreamUseTenantIdForTesting", true};

    ChangeStreamChangeCollectionManager::create(_serviceContext);

    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                getReplCoord(),
                                getStorageInterface(),
                                _writerPool.get(),
                                _observer.get(),
                                OplogWriter::Options());

    std::vector<BSONObj> ops;
    ops.push_back(makeRawInsertOplogEntry(1, kNss1));
    ops.push_back(makeRawInsertOplogEntry(2, kNss2));

    auto returnOpTime = OpTime::parseFromOplogEntry(ops.back()).getValue();
    auto statusWith = oplogWriter.writeOplogBatch(opCtx(), std::move(ops));

    ASSERT_OK(statusWith);
    ASSERT_EQ(returnOpTime, statusWith.getValue());

    // Verify that the batch written to both the oplog and change collection.
    ASSERT_EQ(2, _observer->oplogCollDocsCount);
    ASSERT_EQ(2, _observer->changeCollDocsCount);
}

TEST_F(OplogWriterImplTest, finalizeOplogBatchCorrectlyUpdatesOpTimes) {
    OplogWriterImpl oplogWriter(nullptr,  // executor
                                nullptr,  // writeBuffer
                                nullptr,  // applyBuffer
                                getReplCoord(),
                                getStorageInterface(),
                                _writerPool.get(),
                                &noopOplogWriterObserver,
                                OplogWriter::Options());

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

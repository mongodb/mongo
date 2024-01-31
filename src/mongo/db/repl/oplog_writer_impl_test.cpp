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

#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_batcher_test_fixture.h"
#include "mongo/db/repl/oplog_writer.h"
#include "mongo/db/repl/oplog_writer_impl.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
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

class OplogWriterImplTest : public ServiceContextMongoDTest {
public:
    explicit OplogWriterImplTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true)) {}

    void setUp() override;
    void tearDown() override;

    OperationContext* opCtx() const;

    ReplicationCoordinator* getReplCoord() const;
    StorageInterface* getStorageInterface() const;

protected:
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

DEATH_TEST_F(OplogWriterImplTest, WriteEmptyBatchFails, "!ops.empty()") {
    OplogWriterImpl::NoopObserver noopObserver;
    OplogWriterImpl oplogWriter(getReplCoord(),
                                getStorageInterface(),
                                _writerPool.get(),
                                &noopObserver,
                                OplogWriter::Options());

    // Writing an empty batch should hit an invariant.
    oplogWriter.writeOplogBatch(opCtx(), {}).getStatus().ignore();
}

TEST_F(OplogWriterImplTest, WriteOplogCollectionOnly) {
    OplogWriterImpl oplogWriter(getReplCoord(),
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

    OplogWriterImpl oplogWriter(getReplCoord(),
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

    OplogWriterImpl oplogWriter(getReplCoord(),
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

}  // namespace
}  // namespace repl
}  // namespace mongo

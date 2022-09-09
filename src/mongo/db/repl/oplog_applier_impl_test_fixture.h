/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_txn_record_gen.h"

namespace mongo {

class BSONObj;
class OperationContext;

namespace repl {

/**
 * Test only subclass of OplogApplierImpl that makes applyOplogBatchPerWorker a public method.
 */
class TestApplyOplogGroupApplier : public OplogApplierImpl {
public:
    TestApplyOplogGroupApplier(ReplicationConsistencyMarkers* consistencyMarkers,
                               StorageInterface* storageInterface,
                               const OplogApplier::Options& options)
        : OplogApplierImpl(nullptr,
                           nullptr,
                           nullptr,
                           nullptr,
                           consistencyMarkers,
                           storageInterface,
                           options,
                           nullptr) {}
    using OplogApplierImpl::applyOplogBatchPerWorker;
};

/**
 * OpObserver for OplogApplierImpl test fixture.
 */
class OplogApplierImplOpObserver : public OpObserverNoop {
public:
    /**
     * This function is called whenever OplogApplierImpl inserts documents into a collection.
     */
    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override;

    /**
     * This function is called whenever OplogApplierImpl deletes a document from a collection.
     */
    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const UUID& uuid,
                  StmtId stmtId,
                  const OplogDeleteEntryArgs& args) override;

    /**
     * This function is called whenever OplogApplierImpl updates a document in a collection.
     */
    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) override;

    /**
     * Called when OplogApplierImpl creates a collection.
     */
    void onCreateCollection(OperationContext* opCtx,
                            const CollectionPtr& coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime,
                            bool fromMigrate) override;

    /**
     * Called when OplogApplierImpl renames a collection.
     */
    using OpObserver::onRenameCollection;
    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            const UUID& uuid,
                            const boost::optional<UUID>& dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp) override;

    /**
     * Called when OplogApplierImpl creates an index.
     */
    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) override;

    /**
     * Called when OplogApplierImpl drops an index.
     */
    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const UUID& uuid,
                     const std::string& indexName,
                     const BSONObj& idxDescriptor) override;

    /**
     * Called when OplogApplierImpl performs a CollMod.
     */
    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<IndexCollModInfo> indexInfo) override;

    // Hooks for OpObserver functions. Defaults to a no-op function but may be overridden to
    // check actual documents mutated.
    std::function<void(OperationContext*, const NamespaceString&, const std::vector<BSONObj>&)>
        onInsertsFn;

    std::function<void(OperationContext*,
                       const NamespaceString&,
                       boost::optional<UUID>,
                       StmtId,
                       const OplogDeleteEntryArgs&)>
        onDeleteFn;

    std::function<void(OperationContext*, const OplogUpdateEntryArgs&)> onUpdateFn;

    std::function<void(OperationContext*,
                       const CollectionPtr&,
                       const NamespaceString&,
                       const CollectionOptions&,
                       const BSONObj&)>
        onCreateCollectionFn;

    std::function<void(OperationContext*,
                       const NamespaceString&,
                       const NamespaceString&,
                       boost::optional<UUID>,
                       boost::optional<UUID>,
                       std::uint64_t,
                       bool)>
        onRenameCollectionFn;

    std::function<void(OperationContext*, const NamespaceString&, UUID, BSONObj, bool)>
        onCreateIndexFn;

    std::function<void(OperationContext*,
                       const NamespaceString&,
                       boost::optional<UUID>,
                       const std::string&,
                       const BSONObj&)>
        onDropIndexFn;

    std::function<void(OperationContext*,
                       const NamespaceString&,
                       const UUID&,
                       const BSONObj&,
                       const CollectionOptions&,
                       boost::optional<IndexCollModInfo>)>
        onCollModFn;
};

class OplogApplierImplTest : public ServiceContextMongoDTest {
protected:
    explicit OplogApplierImplTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true)) {}

    void _testApplyOplogEntryOrGroupedInsertsCrudOperation(ErrorCodes::Error expectedError,
                                                           const OplogEntry& op,
                                                           const NamespaceString& targetNss,
                                                           bool expectedApplyOpCalled);

    Status _applyOplogEntryOrGroupedInsertsWrapper(OperationContext* opCtx,
                                                   const OplogEntryOrGroupedInserts& batch,
                                                   OplogApplication::Mode oplogApplicationMode);

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ReplicationConsistencyMarkers> _consistencyMarkers;
    ServiceContext* serviceContext;
    OplogApplierImplOpObserver* _opObserver = nullptr;

    OpTime nextOpTime() {
        static long long lastSecond = 1;
        return OpTime(Timestamp(Seconds(lastSecond++), 0), 1LL);
    }

    void setUp() override;
    void tearDown() override;


    ReplicationCoordinator* getReplCoord() const;
    ReplicationConsistencyMarkers* getConsistencyMarkers() const;
    StorageInterface* getStorageInterface() const;

    Status runOpSteadyState(const OplogEntry& op);
    Status runOpsSteadyState(std::vector<OplogEntry> ops);
    Status runOpInitialSync(const OplogEntry& entry);
    Status runOpsInitialSync(std::vector<OplogEntry> ops);

    UUID kUuid{UUID::gen()};
};

class OplogApplierImplWithFastAutoAdvancingClockTest : public OplogApplierImplTest {
protected:
    OplogApplierImplWithFastAutoAdvancingClockTest()
        : OplogApplierImplTest(
              Options{}.useMockClock(true, Milliseconds{serverGlobalParams.slowMS * 10})) {}
};

class OplogApplierImplWithSlowAutoAdvancingClockTest : public OplogApplierImplTest {
protected:
    OplogApplierImplWithSlowAutoAdvancingClockTest()
        : OplogApplierImplTest(
              Options{}.useMockClock(true, Milliseconds{serverGlobalParams.slowMS / 10})) {}
};

// Utility class to allow easily scanning a collection.  Scans in forward order, returns
// Status::CollectionIsEmpty when scan is exhausted.
class CollectionReader {
public:
    CollectionReader(OperationContext* opCtx, const NamespaceString& nss);

    StatusWith<BSONObj> next();

private:
    AutoGetCollectionForRead _collToScan;
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;
};

Status failedApplyCommand(OperationContext* opCtx,
                          const BSONObj& theOperation,
                          OplogApplication::Mode);

void checkTxnTable(OperationContext* opCtx,
                   const LogicalSessionId& lsid,
                   const TxnNumber& txnNum,
                   const repl::OpTime& expectedOpTime,
                   Date_t expectedWallClock,
                   boost::optional<repl::OpTime> expectedStartOpTime,
                   boost::optional<DurableTxnStateEnum> expectedState);

bool docExists(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc);

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
OplogEntry makeOplogEntry(OpTypeEnum opType,
                          NamespaceString nss,
                          const boost::optional<UUID>& uuid,
                          BSONObj o,
                          boost::optional<BSONObj> o2 = boost::none,
                          boost::optional<bool> fromMigrate = boost::none);

OplogEntry makeOplogEntry(OpTime opTime,
                          OpTypeEnum opType,
                          NamespaceString nss,
                          const boost::optional<UUID>& uuid,
                          BSONObj o,
                          boost::optional<BSONObj> o2 = boost::none,
                          boost::optional<bool> fromMigrate = boost::none);

OplogEntry makeOplogEntry(OpTypeEnum opType, NamespaceString nss, boost::optional<UUID> uuid);

/**
 * Creates collection options suitable for oplog.
 */
CollectionOptions createOplogCollectionOptions();

/*
 * Creates collection options for recording pre-images for testing deletes.
 */
CollectionOptions createRecordPreImageCollectionOptions();

/**
 * Create test collection.
 */
void createCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const CollectionOptions& options);
/**
 * Create test collection with UUID.
 */
UUID createCollectionWithUuid(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Create test database.
 */
void createDatabase(OperationContext* opCtx, StringData dbName);

/**
 * Returns true if collection exists.
 */
bool collectionExists(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Create index on a collection.
 */
void createIndex(OperationContext* opCtx,
                 const NamespaceString& nss,
                 UUID collUUID,
                 const BSONObj& spec);

}  // namespace repl
}  // namespace mongo

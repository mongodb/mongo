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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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
class MONGO_MOD_PUB OplogApplierImplOpObserver : public OpObserverNoop {
public:
    /**
     * This function is called whenever OplogApplierImpl inserts documents into a collection.
     */
    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override;

    /**
     * This function is called whenever OplogApplierImpl deletes a document from a collection.
     */
    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override;

    /**
     * This function is called whenever OplogApplierImpl updates a document in a collection.
     */
    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override;

    /**
     * Called when OplogApplierImpl creates a collection.
     */
    void onCreateCollection(
        OperationContext* opCtx,
        const NamespaceString& collectionName,
        const CollectionOptions& options,
        const BSONObj& idIndex,
        const OplogSlot& createOpTime,
        const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
        bool fromMigrate,
        bool isViewlessTimeseries) override;

    /**
     * Called when OplogApplierImpl renames a collection.
     */
    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            const UUID& uuid,
                            const boost::optional<UUID>& dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp,
                            bool markFromMigrate,
                            bool isViewlessTimeseries) override;

    /**
     * Called when OplogApplierImpl creates an index.
     */
    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       const IndexBuildInfo& indexBuildInfo,
                       bool fromMigrate,
                       bool isViewlessTimeseries) override;

    /**
     * Called when OplogApplierImpl drops an index.
     */
    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const UUID& uuid,
                     const std::string& indexName,
                     const BSONObj& idxDescriptor,
                     bool isViewlessTimeseries) override;

    /**
     * Called when OplogApplierImpl performs a CollMod.
     */
    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<IndexCollModInfo> indexInfo,
                   bool isViewlessTimeseries) override;

    // Hooks for OpObserver functions. Defaults to a no-op function but may be overridden to
    // check actual documents mutated.
    std::function<void(OperationContext*, const NamespaceString&, const std::vector<BSONObj>&)>
        onInsertsFn;

    std::function<void(OperationContext*,
                       const CollectionPtr&,
                       StmtId,
                       const BSONObj&,
                       const OplogDeleteEntryArgs&)>
        onDeleteFn;

    std::function<void(OperationContext*, const OplogUpdateEntryArgs&)> onUpdateFn;

    std::function<void(
        OperationContext*,
        const NamespaceString&,
        const CollectionOptions&,
        const BSONObj&,
        const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier)>
        onCreateCollectionFn;

    std::function<void(OperationContext*,
                       const NamespaceString&,
                       const NamespaceString&,
                       boost::optional<UUID>,
                       boost::optional<UUID>,
                       std::uint64_t,
                       bool,
                       bool)>
        onRenameCollectionFn;

    std::function<void(
        OperationContext*, const NamespaceString&, UUID, const IndexBuildInfo&, bool)>
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

class MONGO_MOD_OPEN OplogApplierImplTest : public ServiceContextMongoDTest {
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
    // TODO SERVER-99926: We disable all lock ordering checks for unit tests to avoid having to fix
    // all instances of lock ordering violations. This is okay to do temporarily as unit tests are
    // not representative of the actual production lock ordering.
    boost::optional<DisableLockerRuntimeOrderingChecks> _disableChecks;

    template <typename T>
    inline void setServerParameter(const std::string& name, T value) {
        _serverParamControllers.push_back(ServerParameterControllerForTest(name, value));
    }

    OpTime nextOpTime() {
        static long long lastSecond = 1;
        return OpTime(Timestamp(Seconds(lastSecond++), 0), 1LL);
    }

    virtual std::unique_ptr<ReplicationCoordinator> makeReplCoord(ServiceContext*);
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

    std::vector<ServerParameterControllerForTest> _serverParamControllers;
};

class OplogApplierImplWithFastAutoAdvancingClockTest : public OplogApplierImplTest {
protected:
    OplogApplierImplWithFastAutoAdvancingClockTest()
        : OplogApplierImplTest(
              Options{}.useMockClock(true, Milliseconds{serverGlobalParams.slowMS.load() * 10})) {}
};

class OplogApplierImplWithSlowAutoAdvancingClockTest : public OplogApplierImplTest {
protected:
    OplogApplierImplWithSlowAutoAdvancingClockTest()
        : OplogApplierImplTest(
              Options{}.useMockClock(true, Milliseconds{serverGlobalParams.slowMS.load() / 10})) {}
};

// Utility class to allow easily scanning a collection.  Scans in forward order, returns
// Status::CollectionIsEmpty when scan is exhausted.
class MONGO_MOD_PUB CollectionReader {
public:
    CollectionReader(OperationContext* opCtx, const NamespaceString& nss);

    StatusWith<BSONObj> next();

private:
    CollectionAcquisition _collToScan;
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;
};

}  // namespace repl

namespace MONGO_MOD_PUB repl {

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
                          boost::optional<bool> fromMigrate = boost::none,
                          OperationSessionInfo sessionInfo = OperationSessionInfo(),
                          boost::optional<RetryImageEnum> needsRetryImage = boost::none);

OplogEntry makeOplogEntry(OpTypeEnum opType, NamespaceString nss, boost::optional<UUID> uuid);

/**
 * Generates a 'create' oplog entry for a new collection.
 */
OplogEntry makeCreateCollectionOplogEntry(
    const OpTime& opTime,
    const NamespaceString& nss,
    const CollectionOptions& collectionOptions,
    const BSONObj& idIndex = BSONObj(),
    boost::optional<CreateCollCatalogIdentifier> createCollCatalogIdentifier = boost::none);
OplogEntry makeCreateCollectionOplogEntry(const OpTime& opTime,
                                          const NamespaceString& nss,
                                          const UUID& uuid = UUID::gen());


/**
 * Creates collection options suitable for oplog.
 */
CollectionOptions createOplogCollectionOptions();

/**
 * Creates collection options for recording change stream pre-images for testing deletes.
 */
CollectionOptions createRecordChangeStreamPreAndPostImagesCollectionOptions();

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

/**
 * Generate a new catalog identifier.
 */
CreateCollCatalogIdentifier newCatalogIdentifier(OperationContext* opCtx,
                                                 const DatabaseName& dbName,
                                                 bool includeIdIndexIdent);


}  // namespace MONGO_MOD_PUB repl
}  // namespace mongo

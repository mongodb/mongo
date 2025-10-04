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

#include "mongo/db/local_catalog/rename_collection.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

/**
 * Mock OpObserver that tracks dropped collections and databases.
 * Since this class is used exclusively to test dropDatabase(), we will also check the drop-pending
 * flag in the Database object being tested (if provided).
 */
class OpObserverMock : public OpObserverNoop {
public:
    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       const IndexBuildInfo& indexBuildInfo,
                       bool fromMigrate,
                       bool isViewlessTimeseries) override;

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           bool fromMigrate,
                           bool isViewlessTimeseries) override;

    void onCommitIndexBuild(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const UUID& collUUID,
                            const UUID& indexBuildUUID,
                            const std::vector<BSONObj>& indexes,
                            bool fromMigrate,
                            bool isViewlessTimeseries) override;

    void onAbortIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           const Status& cause,
                           bool fromMigrate,
                           bool isViewlessTimeseries) override;

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override;

    void onCreateCollection(
        OperationContext* opCtx,
        const NamespaceString& collectionName,
        const CollectionOptions& options,
        const BSONObj& idIndex,
        const OplogSlot& createOpTime,
        const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
        bool fromMigrate,
        bool isViewlessTimeseries) override;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isViewlessTimeseries) override;

    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            const UUID& uuid,
                            const boost::optional<UUID>& dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp,
                            bool markFromMigrate,
                            bool isViewlessTimeseries) override;

    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     const UUID& uuid,
                                     const boost::optional<UUID>& dropTargetUUID,
                                     std::uint64_t numRecords,
                                     bool stayTemp,
                                     bool markFromMigrate,
                                     bool isViewlessTimeseries) override;

    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              const UUID& uuid,
                              const boost::optional<UUID>& dropTargetUUID,
                              bool stayTemp) override;

    // Operations written to the oplog. These are operations for which
    // ReplicationCoordinator::isOplogDisabled() returns false.
    std::vector<std::string> oplogEntries;

    bool onInsertsThrows = false;
    bool onInsertsIsTargetDatabaseExclusivelyLocked = false;

    bool onRenameCollectionCalled = false;
    boost::optional<UUID> onRenameCollectionDropTarget;
    repl::OpTime renameOpTime = {Timestamp(Seconds(100), 1U), 1LL};

    repl::OpTime dropOpTime = {Timestamp(Seconds(100), 1U), 1LL};

private:
    /**
     * Pushes 'operationName' into 'oplogEntries' if we can write to the oplog for this namespace.
     */
    void _logOp(OperationContext* opCtx,
                const NamespaceString& nss,
                const std::string& operationName);
};

void OpObserverMock::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const UUID& uuid,
                                   const IndexBuildInfo& indexBuildInfo,
                                   bool fromMigrate,
                                   bool isViewlessTimeseries) {
    _logOp(opCtx, nss, "index");
    OpObserverNoop::onCreateIndex(
        opCtx, nss, uuid, indexBuildInfo, fromMigrate, isViewlessTimeseries);
}

void OpObserverMock::onStartIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<IndexBuildInfo>& indexes,
                                       bool fromMigrate,
                                       bool isViewlessTimeseries) {
    _logOp(opCtx, nss, "startIndex");
    OpObserverNoop::onStartIndexBuild(
        opCtx, nss, collUUID, indexBuildUUID, indexes, fromMigrate, isViewlessTimeseries);
}

void OpObserverMock::onCommitIndexBuild(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const UUID& collUUID,
                                        const UUID& indexBuildUUID,
                                        const std::vector<BSONObj>& indexes,
                                        bool fromMigrate,
                                        bool isViewlessTimeseries) {
    _logOp(opCtx, nss, "commitIndex");
    OpObserverNoop::onCommitIndexBuild(
        opCtx, nss, collUUID, indexBuildUUID, indexes, fromMigrate, isViewlessTimeseries);
}

void OpObserverMock::onAbortIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       const Status& cause,
                                       bool fromMigrate,
                                       bool isViewlessTimeseries) {
    _logOp(opCtx, nss, "abortIndex");
    OpObserverNoop::onAbortIndexBuild(
        opCtx, nss, collUUID, indexBuildUUID, indexes, cause, fromMigrate, isViewlessTimeseries);
}

void OpObserverMock::onInserts(OperationContext* opCtx,
                               const CollectionPtr& coll,
                               std::vector<InsertStatement>::const_iterator begin,
                               std::vector<InsertStatement>::const_iterator end,
                               const std::vector<RecordId>& recordIds,
                               std::vector<bool> fromMigrate,
                               bool defaultFromMigrate,
                               OpStateAccumulator* opAccumulator) {
    if (onInsertsThrows) {
        uasserted(ErrorCodes::OperationFailed, "insert failed");
    }

    onInsertsIsTargetDatabaseExclusivelyLocked =
        shard_role_details::getLocker(opCtx)->isDbLockedForMode(coll->ns().dbName(), MODE_X);

    _logOp(opCtx, coll->ns(), "inserts");
    OpObserverNoop::onInserts(
        opCtx, coll, begin, end, recordIds, std::move(fromMigrate), defaultFromMigrate);
}

void OpObserverMock::onCreateCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const CollectionOptions& options,
    const BSONObj& idIndex,
    const OplogSlot& createOpTime,
    const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
    bool fromMigrate,
    bool isViewlessTimeseries) {
    _logOp(opCtx, collectionName, "create");
    OpObserverNoop::onCreateCollection(opCtx,
                                       collectionName,
                                       options,
                                       idIndex,
                                       createOpTime,
                                       createCollCatalogIdentifier,
                                       fromMigrate,
                                       isViewlessTimeseries);
}

repl::OpTime OpObserverMock::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const UUID& uuid,
                                              std::uint64_t numRecords,
                                              bool markFromMigrate,
                                              bool isViewlessTimeseries) {
    _logOp(opCtx, collectionName, "drop");
    // If the oplog is not disabled for this namespace, then we need to reserve an op time for the
    // drop.
    if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
        OpObserver::Times::get(opCtx).reservedOpTimes.push_back(dropOpTime);
    }
    auto noopOptime = OpObserverNoop::onDropCollection(
        opCtx, collectionName, uuid, numRecords, markFromMigrate, isViewlessTimeseries);
    invariant(noopOptime.isNull());
    return {};
}

void OpObserverMock::onRenameCollection(OperationContext* opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        const UUID& uuid,
                                        const boost::optional<UUID>& dropTargetUUID,
                                        std::uint64_t numRecords,
                                        bool stayTemp,
                                        bool markFromMigrate,
                                        bool isViewlessTimeseries) {
    preRenameCollection(opCtx,
                        fromCollection,
                        toCollection,
                        uuid,
                        dropTargetUUID,
                        numRecords,
                        stayTemp,
                        markFromMigrate,
                        isViewlessTimeseries);
    OpObserverNoop::onRenameCollection(opCtx,
                                       fromCollection,
                                       toCollection,
                                       uuid,
                                       dropTargetUUID,
                                       numRecords,
                                       stayTemp,
                                       markFromMigrate,
                                       isViewlessTimeseries);
    onRenameCollectionCalled = true;
    onRenameCollectionDropTarget = dropTargetUUID;
}

void OpObserverMock::postRenameCollection(OperationContext* opCtx,
                                          const NamespaceString& fromCollection,
                                          const NamespaceString& toCollection,
                                          const UUID& uuid,
                                          const boost::optional<UUID>& dropTargetUUID,
                                          bool stayTemp) {
    OpObserverNoop::postRenameCollection(
        opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    onRenameCollectionCalled = true;
    onRenameCollectionDropTarget = dropTargetUUID;
}

repl::OpTime OpObserverMock::preRenameCollection(OperationContext* opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 const UUID& uuid,
                                                 const boost::optional<UUID>& dropTargetUUID,
                                                 std::uint64_t numRecords,
                                                 bool stayTemp,
                                                 bool markFromMigrate,
                                                 bool isViewlessTimeseries) {
    _logOp(opCtx, fromCollection, "rename");
    OpObserver::Times::get(opCtx).reservedOpTimes.push_back(renameOpTime);
    OpObserverNoop::preRenameCollection(opCtx,
                                        fromCollection,
                                        toCollection,
                                        uuid,
                                        dropTargetUUID,
                                        numRecords,
                                        stayTemp,
                                        /*markFromMigrate=*/false,
                                        isViewlessTimeseries);
    return {};
}

void OpObserverMock::_logOp(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const std::string& operationName) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }
    oplogEntries.push_back(operationName);
}
class RenameCollectionTest : public ServiceContextMongoDTest {

public:
    static ServiceContext::UniqueOperationContext makeOpCtx();

protected:
    void setUp() override;
    void tearDown() override;

    explicit RenameCollectionTest(Options options = {})
        : ServiceContextMongoDTest(std::move(options)) {}

    ServiceContext::UniqueOperationContext _opCtx;
    repl::ReplicationCoordinatorMock* _replCoord = nullptr;
    OpObserverMock* _opObserver = nullptr;
    NamespaceString _sourceNss{
        NamespaceString::createNamespaceString_forTest(boost::none, "test.foo")};
    NamespaceString _targetNss{
        NamespaceString::createNamespaceString_forTest(boost::none, "test.bar")};
    NamespaceString _targetNssDifferentDb{
        NamespaceString::createNamespaceString_forTest(boost::none, "test2.bar")};
};

// static
ServiceContext::UniqueOperationContext RenameCollectionTest::makeOpCtx() {
    return cc().makeOperationContext();
}

void RenameCollectionTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    _opCtx = cc().makeOperationContext();

    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());

    // Set up ReplicationCoordinator and create oplog.
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
    _replCoord = replCoord.get();
    repl::ReplicationCoordinator::set(service, std::move(replCoord));
    repl::createOplog(_opCtx.get());

    // Ensure that we are primary.
    ASSERT_OK(_replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Use OpObserverMock to track notifications for collection and database drops.
    auto mockObserver = std::make_unique<OpObserverMock>();
    _opObserver = mockObserver.get();
    opObserverRegistry()->addObserver(std::move(mockObserver));
}

void RenameCollectionTest::tearDown() {
    _targetNss = {};
    _sourceNss = {};
    _opObserver = nullptr;
    _replCoord = nullptr;
    _opCtx = {};

    auto service = getServiceContext();
    repl::StorageInterface::set(service, {});

    ServiceContextMongoDTest::tearDown();
}

/**
 * Returns true if collection exists.
 */
bool _collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
    const auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    return coll.exists();
}

/**
 * Creates a collection without any namespace restrictions.
 */
void _createCollection(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const CollectionOptions options = {}) {
    writeConflictRetry(opCtx, "_createCollection", nss, [=] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(opCtx);
        ASSERT_TRUE(db) << "Cannot create collection " << nss.toStringForErrorMsg()
                        << " because database " << nss.toStringForErrorMsg() << " does not exist.";

        WriteUnitOfWork wuow(opCtx);
        ASSERT_TRUE(db->createCollection(opCtx, nss, options))
            << "Failed to create collection " << nss.toStringForErrorMsg()
            << " due to unknown error.";
        wuow.commit();
    });

    ASSERT_TRUE(_collectionExists(opCtx, nss));
}

/**
 * Returns a collection options with a generated UUID.
 */
CollectionOptions _makeCollectionOptionsWithUuid() {
    CollectionOptions options;
    options.uuid = UUID::gen();
    return options;
}

/**
 * Creates a collection with UUID and returns the UUID.
 */
UUID _createCollectionWithUUID(OperationContext* opCtx, const NamespaceString& nss) {
    const auto options = _makeCollectionOptionsWithUuid();
    _createCollection(opCtx, nss, options);
    return options.uuid.value();
}

/**
 * Returns collection options.
 */
CollectionOptions _getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
    const auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);

    ASSERT_TRUE(coll.exists()) << "Unable to get collections options for "
                               << nss.toStringForErrorMsg()
                               << " because collection does not exist.";
    return coll.getCollectionPtr()->getCollectionOptions();
}

/**
 * Returns UUID of collection.
 */
UUID _getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    auto options = _getCollectionOptions(opCtx, nss);
    ASSERT_TRUE(options.uuid);
    return *(options.uuid);
}

/**
 * Get collection namespace by UUID.
 */
NamespaceString _getCollectionNssFromUUID(OperationContext* opCtx, const UUID& uuid) {
    const Collection* source = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, uuid);
    return source ? source->ns() : NamespaceString::kEmpty;
}

/**
 * Returns true if namespace refers to a temporary collection.
 */
bool _isTempCollection(OperationContext* opCtx, const NamespaceString& nss) {
    ASSERT_TRUE(_collectionExists(opCtx, nss))
        << "Unable to check if " << nss.toStringForErrorMsg()
        << " is a temporary collection because collection does not exist.";
    auto options = _getCollectionOptions(opCtx, nss);
    return options.temp;
}

/**
 * Creates an index on an empty collection using the given index name with a bogus key spec.
 */
void _createIndexOnEmptyCollection(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::string& indexName) {
    writeConflictRetry(opCtx, "_createIndexOnEmptyCollection", nss, [=] {
        AutoGetCollection collection(opCtx, nss, MODE_X);
        ASSERT_TRUE(collection) << "Cannot create index on empty collection "
                                << nss.toStringForErrorMsg() << " because collection "
                                << nss.toStringForErrorMsg() << " does not exist.";

        auto indexInfoObj = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key"
                                     << BSON("a" << 1) << "name" << indexName);

        WriteUnitOfWork wuow(opCtx);
        CollectionWriter writer{opCtx, collection};

        auto indexCatalog = writer.getWritableCollection(opCtx)->getIndexCatalog();
        ASSERT_OK(indexCatalog
                      ->createIndexOnEmptyCollection(
                          opCtx, writer.getWritableCollection(opCtx), indexInfoObj)
                      .getStatus());
        wuow.commit();
    });

    ASSERT_TRUE(_collectionExists(opCtx, nss));
}

/**
 * Inserts a single document into a collection.
 */
void _insertDocument(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc) {
    writeConflictRetry(opCtx, "_insertDocument", nss, [=] {
        AutoGetCollection collection(opCtx, nss, MODE_X);
        ASSERT_TRUE(collection) << "Cannot insert document " << doc << " into collection "
                                << nss.toStringForErrorMsg() << " because collection "
                                << nss.toStringForErrorMsg() << " does not exist.";

        WriteUnitOfWork wuow(opCtx);
        OpDebug* const opDebug = nullptr;
        ASSERT_OK(
            collection_internal::insertDocument(opCtx, *collection, InsertStatement(doc), opDebug));
        wuow.commit();
    });
}

TEST_F(RenameCollectionTest, RenameCollectionReturnsNamespaceNotFoundIfDatabaseDoesNotExist) {
    ASSERT_FALSE(AutoGetDb(_opCtx.get(), _sourceNss.dbName(), MODE_X).getDb());
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  renameCollection(_opCtx.get(), _sourceNss, _targetNss, {}));
}

TEST_F(RenameCollectionTest, RenameCollectionReturnsNotWritablePrimaryIfNotPrimary) {
    _createCollection(_opCtx.get(), _sourceNss);
    ASSERT_OK(_replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(_replCoord->canAcceptWritesForDatabase(_opCtx.get(), _sourceNss.dbName()));
    ASSERT_EQUALS(ErrorCodes::NotWritablePrimary,
                  renameCollection(_opCtx.get(), _sourceNss, _targetNss, {}));
}

TEST_F(RenameCollectionTest, TargetCollectionNameLong) {
    _createCollection(_opCtx.get(), _sourceNss);
    const std::string targetCollectionName(255, 'a');
    NamespaceString longTargetNss = NamespaceString::createNamespaceString_forTest(
        _sourceNss.db_forTest(), targetCollectionName);
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, longTargetNss, {}));
}

TEST_F(RenameCollectionTest, LongIndexNameAllowedForTargetCollection) {
    ASSERT_GREATER_THAN(_targetNssDifferentDb.size(), _sourceNss.size());

    _createCollection(_opCtx.get(), _sourceNss);
    std::size_t longIndexLength = 500;
    const std::string indexName(longIndexLength, 'a');
    _createIndexOnEmptyCollection(_opCtx.get(), _sourceNss, indexName);
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
}

TEST_F(RenameCollectionTest, LongIndexNameAllowedForTemporaryCollectionForRenameAcrossDatabase) {
    ASSERT_GREATER_THAN(_targetNssDifferentDb.size(), _sourceNss.size());

    // Using XXXXX to check namespace length. Each 'X' will be replaced by a random character in
    // renameCollection().
    const NamespaceString tempNss = NamespaceString::createNamespaceString_forTest(
        _targetNssDifferentDb.getSisterNS("tmpXXXXX.renameCollection"));

    _createCollection(_opCtx.get(), _sourceNss);
    std::size_t longIndexLength = 500;
    const std::string indexName(longIndexLength, 'a');
    _createIndexOnEmptyCollection(_opCtx.get(), _sourceNss, indexName);
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseWithUuid) {
    auto options = _makeCollectionOptionsWithUuid();
    _createCollection(_opCtx.get(), _sourceNss, options);
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNss));
    ASSERT(options.uuid);
    ASSERT_NOT_EQUALS(*options.uuid, _getCollectionUuid(_opCtx.get(), _targetNssDifferentDb));
}

TEST_F(RenameCollectionTest, RenameCollectionToItselfByNsForApplyOps) {
    auto uuid = _createCollectionWithUUID(_opCtx.get(), _sourceNss);
    auto cmd = BSON("renameCollection" << _sourceNss.ns_forTest() << "to" << _sourceNss.ns_forTest()
                                       << "dropTarget" << true);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), uuid, boost::none, cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _sourceNss));
}

TEST_F(RenameCollectionTest, RenameCollectionToItselfByUUIDForApplyOps) {
    auto uuid = _createCollectionWithUUID(_opCtx.get(), _targetNss);
    auto cmd = BSON("renameCollection" << _sourceNss.ns_forTest() << "to" << _targetNss.ns_forTest()
                                       << "dropTarget" << true);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), uuid, boost::none, cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss));
}

TEST_F(RenameCollectionTest, RenameCollectionByUUIDRatherThanNsForApplyOps) {
    auto realRenameFromNss = NamespaceString::createNamespaceString_forTest("test.bar2");
    auto uuid = _createCollectionWithUUID(_opCtx.get(), realRenameFromNss);
    auto cmd = BSON("renameCollection" << _sourceNss.ns_forTest() << "to" << _targetNss.ns_forTest()
                                       << "dropTarget" << true);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), uuid, boost::none, cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetByUUIDTargetDoesNotExist) {
    const auto& collA = NamespaceString::createNamespaceString_forTest("test.A");
    const auto& collB = NamespaceString::createNamespaceString_forTest("test.B");
    const auto& collC = NamespaceString::createNamespaceString_forTest("test.C");
    auto collAUUID = _createCollectionWithUUID(_opCtx.get(), collA);
    auto collCUUID = _createCollectionWithUUID(_opCtx.get(), collC);
    // Rename A to B, drop C, where B is not an existing collection
    auto cmd = BSON("renameCollection" << collA.ns_forTest() << "to" << collB.ns_forTest()
                                       << "dropTarget" << collCUUID);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), collAUUID, boost::none, cmd, {}));
    // A and C should be dropped
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collA));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collC));
    // B (originally A) should exist
    ASSERT_TRUE(_collectionExists(_opCtx.get(), collB));
    // collAUUID should be associated with collB's NamespaceString in the CollectionCatalog.
    auto newCollNS = _getCollectionNssFromUUID(_opCtx.get(), collAUUID);
    ASSERT_TRUE(newCollNS.isValid());
    ASSERT_EQUALS(newCollNS, collB);
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetByUUIDTargetExists) {
    const auto& collA = NamespaceString::createNamespaceString_forTest("test.A");
    const auto& collB = NamespaceString::createNamespaceString_forTest("test.B");
    const auto& collC = NamespaceString::createNamespaceString_forTest("test.C");
    auto collAUUID = _createCollectionWithUUID(_opCtx.get(), collA);
    auto collBUUID = _createCollectionWithUUID(_opCtx.get(), collB);
    auto collCUUID = _createCollectionWithUUID(_opCtx.get(), collC);
    // Rename A to B, drop C, where B is an existing collection
    // B should be kept but with a temporary name
    auto cmd = BSON("renameCollection" << collA.ns_forTest() << "to" << collB.ns_forTest()
                                       << "dropTarget" << collCUUID);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), collAUUID, boost::none, cmd, {}));
    // A and C should be dropped
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collA));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collC));
    // B (originally A) should exist
    ASSERT_TRUE(_collectionExists(_opCtx.get(), collB));
    // The original B should exist too, but with a temporary name
    const auto& tmpB =
        CollectionCatalog::get(_opCtx.get())->lookupNSSByUUID(_opCtx.get(), collBUUID);
    ASSERT(tmpB);
    ASSERT_TRUE(tmpB->coll().starts_with("tmp"));
    ASSERT_TRUE(*tmpB != collB);
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsDropTargetByUUIDTargetExistsButTemporarily) {

    const auto& collA = NamespaceString::createNamespaceString_forTest("test.A");
    const auto& collB = NamespaceString::createNamespaceString_forTest("test.B");
    const auto& collC = NamespaceString::createNamespaceString_forTest("test.C");

    CollectionOptions collectionOptions = _makeCollectionOptionsWithUuid();
    collectionOptions.temp = true;
    _createCollection(_opCtx.get(), collB, collectionOptions);
    auto collBUUID = _getCollectionUuid(_opCtx.get(), collB);

    auto collAUUID = _createCollectionWithUUID(_opCtx.get(), collA);
    auto collCUUID = _createCollectionWithUUID(_opCtx.get(), collC);
    // Rename A to B, drop C, where B is an existing collection
    // B should be kept but with a temporary name
    auto cmd = BSON("renameCollection" << collA.ns_forTest() << "to" << collB.ns_forTest()
                                       << "dropTarget" << collCUUID);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), collAUUID, boost::none, cmd, {}));
    // A and C should be dropped
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collA));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collC));
    // B (originally A) should exist
    ASSERT_TRUE(_collectionExists(_opCtx.get(), collB));
    // The original B should exist too, but with a temporary name
    const auto& tmpB =
        CollectionCatalog::get(_opCtx.get())->lookupNSSByUUID(_opCtx.get(), collBUUID);
    ASSERT(tmpB);
    ASSERT_TRUE(*tmpB != collB);
    ASSERT_TRUE(tmpB->coll().starts_with("tmp"));
    ASSERT_TRUE(_isTempCollection(_opCtx.get(), *tmpB));
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsDropTargetByUUIDTargetExistsButRealDropTargetDoesNotExist) {
    const auto& collA = NamespaceString::createNamespaceString_forTest("test.A");
    const auto& collB = NamespaceString::createNamespaceString_forTest("test.B");
    auto collAUUID = _createCollectionWithUUID(_opCtx.get(), collA);
    auto collBUUID = _createCollectionWithUUID(_opCtx.get(), collB);
    auto collCUUID = UUID::gen();
    // Rename A to B, drop C, where B is an existing collection
    // B should be kept but with a temporary name
    auto cmd = BSON("renameCollection" << collA.ns_forTest() << "to" << collB.ns_forTest()
                                       << "dropTarget" << collCUUID);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), collAUUID, boost::none, cmd, {}));
    // A and C should be dropped
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collA));
    // B (originally A) should exist
    ASSERT_TRUE(_collectionExists(_opCtx.get(), collB));
    // The original B should exist too, but with a temporary name
    const auto& tmpB =
        CollectionCatalog::get(_opCtx.get())->lookupNSSByUUID(_opCtx.get(), collBUUID);
    ASSERT(tmpB);
    ASSERT_TRUE(*tmpB != collB);
    ASSERT_TRUE(tmpB->coll().starts_with("tmp"));
}

TEST_F(RenameCollectionTest,
       RenameCollectionReturnsNamespaceExitsIfTargetExistsAndDropTargetIsFalse) {
    _createCollection(_opCtx.get(), _sourceNss);
    _createCollection(_opCtx.get(), _targetNss);
    RenameCollectionOptions options;
    ASSERT_FALSE(options.dropTarget);
    ASSERT_EQUALS(ErrorCodes::NamespaceExists,
                  renameCollection(_opCtx.get(), _sourceNss, _targetNss, options));
}

TEST_F(RenameCollectionTest,
       RenameCollectionOverridesDropTargetIfTargetCollectionIsMissingAndDropTargetIsTrue) {
    _createCollectionWithUUID(_opCtx.get(), _sourceNss);
    RenameCollectionOptions options;
    options.dropTarget = true;
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNss, options));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNss))
        << "source collection " << _sourceNss.toStringForErrorMsg()
        << " still exists after successful rename";
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss))
        << "target collection " << _targetNss.toStringForErrorMsg()
        << " missing after successful rename";

    ASSERT_TRUE(_opObserver->onRenameCollectionCalled);
    ASSERT_FALSE(_opObserver->onRenameCollectionDropTarget);
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsRejectsRenameOpTimeIfWritesAreReplicated) {
    ASSERT_TRUE(_opCtx->writesAreReplicated());

    _createCollection(_opCtx.get(), _sourceNss);
    auto cmd =
        BSON("renameCollection" << _sourceNss.ns_forTest() << "to" << _targetNss.ns_forTest());
    auto renameOpTime = _opObserver->renameOpTime;
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        renameCollectionForApplyOps(_opCtx.get(), boost::none, boost::none, cmd, renameOpTime));
}

DEATH_TEST_F(RenameCollectionTest,
             RenameCollectionForApplyOpsTriggersFatalAssertionIfLogOpReturnsValidOpTime,
             "unexpected renameCollection oplog entry written to the oplog") {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    ASSERT_FALSE(_opCtx->writesAreReplicated());

    _createCollection(_opCtx.get(), _sourceNss);
    auto dropTargetUUID = _createCollectionWithUUID(_opCtx.get(), _targetNss);
    auto cmd = BSON("renameCollection" << _sourceNss.ns_forTest() << "to" << _targetNss.ns_forTest()
                                       << "dropTarget" << dropTargetUUID);

    repl::OpTime renameOpTime = {Timestamp(Seconds(200), 1U), 1LL};
    ASSERT_OK(
        renameCollectionForApplyOps(_opCtx.get(), boost::none, boost::none, cmd, renameOpTime));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsSourceAndTargetDoNotExist) {
    auto uuid = UUID::gen();
    auto cmd = BSON("renameCollection" << _sourceNss.ns_forTest() << "to" << _targetNss.ns_forTest()
                                       << "dropTarget"
                                       << "true");
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), uuid, boost::none, cmd, {}));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNss));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _targetNss));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetEvenIfSourceDoesNotExist) {
    _createCollectionWithUUID(_opCtx.get(), _targetNss);
    auto missingSourceNss = NamespaceString::createNamespaceString_forTest("test.bar2");
    auto uuid = UUID::gen();
    auto cmd = BSON("renameCollection" << missingSourceNss.ns_forTest() << "to"
                                       << _targetNss.ns_forTest() << "dropTarget"
                                       << "true");
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), uuid, boost::none, cmd, {}));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _targetNss));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetByUUIDEvenIfSourceDoesNotExist) {
    auto missingSourceNss = NamespaceString::createNamespaceString_forTest("test.bar2");
    auto dropTargetNss = NamespaceString::createNamespaceString_forTest("test.bar3");
    _createCollectionWithUUID(_opCtx.get(), _targetNss);
    auto dropTargetUUID = _createCollectionWithUUID(_opCtx.get(), dropTargetNss);
    auto uuid = UUID::gen();
    auto cmd =
        BSON("renameCollection" << missingSourceNss.ns_forTest() << "to" << _targetNss.ns_forTest()
                                << "dropTarget" << dropTargetUUID);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), uuid, boost::none, cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), dropTargetNss));
}

void _testRenameCollectionStayTemp(OperationContext* opCtx,
                                   const NamespaceString& sourceNss,
                                   const NamespaceString& targetNss,
                                   bool stayTemp,
                                   bool isSourceCollectionTemporary) {
    CollectionOptions collectionOptions;
    collectionOptions.temp = isSourceCollectionTemporary;
    _createCollection(opCtx, sourceNss, collectionOptions);

    RenameCollectionOptions options;
    options.stayTemp = stayTemp;
    ASSERT_OK(renameCollection(opCtx, sourceNss, targetNss, options));
    ASSERT_FALSE(_collectionExists(opCtx, sourceNss))
        << "source collection " << sourceNss.toStringForErrorMsg()
        << " still exists after successful rename";

    if (!isSourceCollectionTemporary) {
        ASSERT_FALSE(_isTempCollection(opCtx, targetNss))
            << "target collection " << targetNss.toStringForErrorMsg()
            << " cannot not be temporary after rename if source collection is not temporary.";
    } else if (stayTemp) {
        ASSERT_TRUE(_isTempCollection(opCtx, targetNss))
            << "target collection " << targetNss.toStringForErrorMsg()
            << " is no longer temporary after rename with stayTemp set to true.";
    } else {
        ASSERT_FALSE(_isTempCollection(opCtx, targetNss))
            << "target collection " << targetNss.toStringForErrorMsg()
            << " still temporary after rename with stayTemp set to false.";
    }
}

TEST_F(RenameCollectionTest, RenameSameDatabaseStayTempFalse) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNss, false, true);
}

TEST_F(RenameCollectionTest, RenameSameDatabaseStayTempTrue) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNss, true, true);
}

TEST_F(RenameCollectionTest, RenameDifferentDatabaseStayTempFalse) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNssDifferentDb, false, true);
}

TEST_F(RenameCollectionTest, RenameDifferentDatabaseStayTempTrue) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNssDifferentDb, true, true);
}

TEST_F(RenameCollectionTest, RenameSameDatabaseStayTempFalseSourceNotTemporary) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNss, false, false);
}

TEST_F(RenameCollectionTest, RenameSameDatabaseStayTempTrueSourceNotTemporary) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNss, true, false);
}

TEST_F(RenameCollectionTest, RenameDifferentDatabaseStayTempFalseSourceNotTemporary) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNssDifferentDb, false, false);
}

TEST_F(RenameCollectionTest, RenameDifferentDatabaseStayTempTrueSourceNotTemporary) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNssDifferentDb, true, false);
}

/**
 * Checks oplog entries written by the OpObserver to the oplog.
 */
void _checkOplogEntries(const std::vector<std::string>& actualOplogEntries,
                        const std::vector<std::string>& expectedOplogEntries) {
    std::string actualOplogEntriesStr;
    str::joinStringDelim(actualOplogEntries, &actualOplogEntriesStr, ',');
    std::string expectedOplogEntriesStr;
    str::joinStringDelim(expectedOplogEntries, &expectedOplogEntriesStr, ',');
    ASSERT_EQUALS(expectedOplogEntries.size(), actualOplogEntries.size())

        << "Incorrect number of oplog entries written to oplog. Actual: " << actualOplogEntriesStr
        << ". Expected: " << expectedOplogEntriesStr;
    std::vector<std::string>::size_type i = 0;
    for (const auto& actualOplogEntry : actualOplogEntries) {
        const auto& expectedOplogEntry = expectedOplogEntries[i++];
        ASSERT_EQUALS(expectedOplogEntry, actualOplogEntry)
            << "Mismatch in oplog entry at index " << i << ". Actual: " << actualOplogEntriesStr
            << ". Expected: " << expectedOplogEntriesStr;
    }
}

/**
 * Runs a rename across database operation and checks oplog entries written to the oplog.
 */
void _testRenameCollectionAcrossDatabaseOplogEntries(
    OperationContext* opCtx,
    const NamespaceString& sourceNss,
    const NamespaceString& targetNss,
    std::vector<std::string>* oplogEntries,
    bool forApplyOps,
    const std::vector<std::string>& expectedOplogEntries) {
    ASSERT_NOT_EQUALS(sourceNss.db_forTest(), targetNss.db_forTest());
    _createCollection(opCtx, sourceNss);
    _createIndexOnEmptyCollection(opCtx, sourceNss, "a_1");
    _insertDocument(opCtx, sourceNss, BSON("_id" << 0));
    oplogEntries->clear();
    if (forApplyOps) {
        auto cmd = BSON("renameCollection" << sourceNss.ns_forTest() << "to"
                                           << targetNss.ns_forTest() << "dropTarget" << true);
        ASSERT_OK(renameCollectionForApplyOps(opCtx, boost::none, sourceNss.tenantId(), cmd, {}));
    } else {
        RenameCollectionOptions options;
        options.dropTarget = true;
        ASSERT_OK(renameCollection(opCtx, sourceNss, targetNss, options));
    }
    _checkOplogEntries(*oplogEntries, expectedOplogEntries);
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseOplogEntries) {
    bool forApplyOps = false;
    std::vector<std::string> expectedOplogEntries;
    // Empty collections generate createIndexes oplog entry even if the node
    // supports 2 phase index build.
    expectedOplogEntries = {"create", "index", "inserts", "rename", "drop"};
    _testRenameCollectionAcrossDatabaseOplogEntries(_opCtx.get(),
                                                    _sourceNss,
                                                    _targetNssDifferentDb,
                                                    &_opObserver->oplogEntries,
                                                    forApplyOps,
                                                    expectedOplogEntries);
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsAcrossDatabaseOplogEntries) {
    bool forApplyOps = true;
    std::vector<std::string> expectedOplogEntries;
    // Empty collections generate createIndexes oplog entry even if the node
    // supports 2 phase index build.
    expectedOplogEntries = {"create", "index", "inserts", "rename", "drop"};
    _testRenameCollectionAcrossDatabaseOplogEntries(_opCtx.get(),
                                                    _sourceNss,
                                                    _targetNssDifferentDb,
                                                    &_opObserver->oplogEntries,
                                                    forApplyOps,
                                                    expectedOplogEntries);
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseOplogEntriesDropTarget) {
    _createCollection(_opCtx.get(), _targetNssDifferentDb);
    bool forApplyOps = false;
    std::vector<std::string> expectedOplogEntries;
    // Empty collections generate createIndexes oplog entry even if the node
    // supports 2 phase index build.
    expectedOplogEntries = {"create", "index", "inserts", "rename", "drop"};
    _testRenameCollectionAcrossDatabaseOplogEntries(_opCtx.get(),
                                                    _sourceNss,
                                                    _targetNssDifferentDb,
                                                    &_opObserver->oplogEntries,
                                                    forApplyOps,
                                                    expectedOplogEntries);
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsAcrossDatabaseOplogEntriesDropTarget) {
    _createCollection(_opCtx.get(), _targetNssDifferentDb);
    bool forApplyOps = true;
    std::vector<std::string> expectedOplogEntries;
    // Empty collections generate createIndexes oplog entry even if the node
    // supports 2 phase index build.
    expectedOplogEntries = {"create", "index", "inserts", "rename", "drop"};
    _testRenameCollectionAcrossDatabaseOplogEntries(_opCtx.get(),
                                                    _sourceNss,
                                                    _targetNssDifferentDb,
                                                    &_opObserver->oplogEntries,
                                                    forApplyOps,
                                                    expectedOplogEntries);
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseOplogEntriesWritesNotReplicated) {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    bool forApplyOps = false;
    _testRenameCollectionAcrossDatabaseOplogEntries(_opCtx.get(),
                                                    _sourceNss,
                                                    _targetNssDifferentDb,
                                                    &_opObserver->oplogEntries,
                                                    forApplyOps,
                                                    {});
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsAcrossDatabaseOplogEntriesWritesNotReplicated) {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    bool forApplyOps = true;
    _testRenameCollectionAcrossDatabaseOplogEntries(_opCtx.get(),
                                                    _sourceNss,
                                                    _targetNssDifferentDb,
                                                    &_opObserver->oplogEntries,
                                                    forApplyOps,
                                                    {});
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseDropsTemporaryCollectionOnException) {
    _createCollection(_opCtx.get(), _sourceNss);
    _createIndexOnEmptyCollection(_opCtx.get(), _sourceNss, "a_1");
    _insertDocument(_opCtx.get(), _sourceNss, BSON("_id" << 0));
    _opObserver->onInsertsThrows = true;
    _opObserver->oplogEntries.clear();
    ASSERT_THROWS_CODE(renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    std::vector<std::string> expectedOplogEntries;
    // Empty Collections generate createIndexes oplog entry even if the node
    // supports 2 phase index build.
    expectedOplogEntries = {"create", "index", "drop"};
    _checkOplogEntries(_opObserver->oplogEntries, expectedOplogEntries);
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabasesWithoutLocks) {
    _createCollection(_opCtx.get(), _sourceNss);
    _insertDocument(_opCtx.get(), _sourceNss, BSON("_id" << 0));
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
    ASSERT_FALSE(_opObserver->onInsertsIsTargetDatabaseExclusivelyLocked);
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabasesWithLocks) {
    // This simulates the case when renameCollection is called using the applyOps command (different
    // from secondary oplog application).
    _createCollection(_opCtx.get(), _sourceNss);
    _insertDocument(_opCtx.get(), _sourceNss, BSON("_id" << 0));
    Lock::DBLock sourceLk(_opCtx.get(), _sourceNss.dbName(), MODE_X);
    Lock::DBLock targetLk(_opCtx.get(), _targetNssDifferentDb.dbName(), MODE_X);
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
    ASSERT_TRUE(_opObserver->onInsertsIsTargetDatabaseExclusivelyLocked);
}

TEST_F(RenameCollectionTest, FailRenameCollectionFromReplicatedToUnreplicatedDB) {
    NamespaceString sourceNss = NamespaceString::createNamespaceString_forTest("foo.isReplicated");
    NamespaceString targetNss =
        NamespaceString::createNamespaceString_forTest("local.isUnreplicated");

    _createCollection(_opCtx.get(), sourceNss);

    ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                  renameCollection(_opCtx.get(), sourceNss, targetNss, {}));
}

TEST_F(RenameCollectionTest, FailRenameCollectionFromUnreplicatedToReplicatedDB) {
    NamespaceString sourceNss =
        NamespaceString::createNamespaceString_forTest("foo.system.profile");
    NamespaceString targetNss = NamespaceString::createNamespaceString_forTest("foo.bar");

    _createCollection(_opCtx.get(), sourceNss);

    ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                  renameCollection(_opCtx.get(), sourceNss, targetNss, {}));
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsReturnsInvalidNamespaceIfTargetNamespaceIsInvalid) {
    _createCollection(_opCtx.get(), _sourceNss);

    // Create a namespace that is not in the form "database.collection".
    NamespaceString invalidTargetNss =
        NamespaceString::createNamespaceString_forTest("invalidNamespace");

    auto cmd = BSON("renameCollection" << _sourceNss.ns_forTest() << "to"
                                       << invalidTargetNss.ns_forTest());

    ASSERT_EQUALS(ErrorCodes::InvalidNamespace,
                  renameCollectionForApplyOps(_opCtx.get(), boost::none, boost::none, cmd, {}));
}

TEST_F(RenameCollectionTest, FailRenameCollectionFromSystemJavascript) {
    NamespaceString sourceNss = NamespaceString::createNamespaceString_forTest(
        "foo", NamespaceString::kSystemDotJavascriptCollectionName);
    NamespaceString targetNss = NamespaceString::createNamespaceString_forTest("foo.bar");

    _createCollection(_opCtx.get(), sourceNss);

    auto status = renameCollection(_opCtx.get(), sourceNss, targetNss, {});

    ASSERT_EQUALS(ErrorCodes::IllegalOperation, status);
    ASSERT_STRING_SEARCH_REGEX(status.reason(), "renaming system.js.*not allowed");

    // Used for sharded rename.
    ASSERT_THROWS_CODE_AND_WHAT(
        validateNamespacesForRenameCollection(_opCtx.get(), sourceNss, targetNss),
        AssertionException,
        ErrorCodes::IllegalOperation,
        status.reason());
}

TEST_F(RenameCollectionTest, FailRenameCollectionToSystemJavascript) {
    NamespaceString sourceNss = NamespaceString::createNamespaceString_forTest("foo.bar");
    NamespaceString targetNss = NamespaceString::createNamespaceString_forTest(
        "foo", NamespaceString::kSystemDotJavascriptCollectionName);

    _createCollection(_opCtx.get(), sourceNss);

    auto status = renameCollection(_opCtx.get(), sourceNss, targetNss, {});

    ASSERT_EQUALS(ErrorCodes::IllegalOperation, status);
    ASSERT_STRING_SEARCH_REGEX(status.reason(), "renaming to system.js.*not allowed");

    // Used for sharded rename.
    ASSERT_THROWS_CODE_AND_WHAT(
        validateNamespacesForRenameCollection(_opCtx.get(), sourceNss, targetNss),
        AssertionException,
        ErrorCodes::IllegalOperation,
        status.reason());
}


class RenameCollectionTestMultitenancy : public RenameCollectionTest {

private:
    void setUp() override;

protected:
    TenantId _tenantId{TenantId(OID::gen())};
    TenantId _otherTenantId{TenantId(OID::gen())};
    std::string _sourceNs{"test.foo"};
    std::string _otherNs{"test.bar"};

    NamespaceString _sourceNssTid =
        (NamespaceString::createNamespaceString_forTest(_tenantId, _sourceNs));
};

void RenameCollectionTestMultitenancy::setUp() {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    RenameCollectionTest::setUp();

    _createCollection(_opCtx.get(), _sourceNssTid);
    _createIndexOnEmptyCollection(_opCtx.get(), _sourceNssTid, "a_1");
    _insertDocument(_opCtx.get(), _sourceNssTid, BSON("_id" << 0));

    ASSERT_TRUE(_collectionExists(_opCtx.get(), _sourceNssTid));
}

TEST_F(RenameCollectionTestMultitenancy, RenameCollectionForApplyOps) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    const NamespaceString targetNssTid =
        NamespaceString::createNamespaceString_forTest(_tenantId, _otherNs);
    ASSERT_NOT_EQUALS(_sourceNssTid, targetNssTid);

    // A tid field supersedes tenantIds maintained in source or target, ie. we don't take into
    // account the tenantId from the _sourceNssTid or targetNssTid objects, only the _tenantId obj.
    // This makes it impossible to mismatch tenantIds between source and target if the only tenantId
    // being considered is common to both; if it doesn't match the source, rename will fail being
    // unable to locate the source collection.  If the targetNss tenantId doesn't match, well we're
    // going to rename the db/collection within the _tenantId, so this effectively ensures it isn't
    // possible to rename across tenantIds.
    auto cmd = BSON("renameCollection" << _sourceNssTid.toString_forTest() << "to"
                                       << targetNssTid.toString_forTest());

    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), boost::none, _tenantId, cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), targetNssTid));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNssTid));
}

TEST_F(RenameCollectionTestMultitenancy, RenameCollectionForApplyOpsCommonRandomTid) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    const NamespaceString targetNssTid =
        NamespaceString::createNamespaceString_forTest(_otherTenantId, _sourceNs);
    ASSERT_NOT_EQUALS(_sourceNssTid, targetNssTid);

    // This test only has a single tenantId that belongs to neither source nor target.
    auto cmd = BSON("renameCollection" << _sourceNssTid.toString_forTest() << "to"
                                       << targetNssTid.toString_forTest());

    ASSERT_OK(
        renameCollectionForApplyOps(_opCtx.get(), boost::none, TenantId(OID::gen()), cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _sourceNssTid));
}

TEST_F(RenameCollectionTestMultitenancy, RenameCollectionForApplyOpsCommonTid) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    const NamespaceString targetNssTid =
        NamespaceString::createNamespaceString_forTest(_otherTenantId, _otherNs);
    ASSERT_NOT_EQUALS(_sourceNssTid, targetNssTid);

    // A tid field supersedes tenantIds maintained in source or target. See above.
    auto cmd = BSON("renameCollection" << _sourceNssTid.toString_forTest() << "to"
                                       << targetNssTid.toString_forTest());
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), boost::none, _tenantId, cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(),
                                  NamespaceString::createNamespaceString_forTest(
                                      _tenantId, targetNssTid.toString_forTest())));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNssTid));
}

TEST_F(RenameCollectionTestMultitenancy, RenameCollectionForApplyOpsSourceExistsOnWrongTenant) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    const NamespaceString otherSourceNssTid =
        NamespaceString::createNamespaceString_forTest(_otherTenantId, _sourceNs);
    const NamespaceString targetNssTid =
        NamespaceString::createNamespaceString_forTest(_otherTenantId, _otherNs);
    ASSERT_NOT_EQUALS(otherSourceNssTid, targetNssTid);

    // A tid field supersedes tenantIds maintained in source or target. See above.
    auto cmd = BSON("renameCollection" << otherSourceNssTid.toString_forTest() << "to"
                                       << targetNssTid.toString_forTest());
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  renameCollectionForApplyOps(_opCtx.get(), boost::none, _otherTenantId, cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _sourceNssTid));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), otherSourceNssTid));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), targetNssTid));
}

TEST_F(RenameCollectionTestMultitenancy,
       RenameCollectionForApplyOpsSourceExistsOnWrongTenantRequireTenantIdFalse) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);

    const NamespaceString otherSourceNssTid =
        NamespaceString::createNamespaceString_forTest(_otherTenantId, _sourceNs);
    const NamespaceString targetNssTid =
        NamespaceString::createNamespaceString_forTest(_otherTenantId, _otherNs);
    ASSERT_NOT_EQUALS(otherSourceNssTid, targetNssTid);

    // A tid field supersedes tenantIds maintained in source or target. See above.
    auto cmd = BSON("renameCollection" << otherSourceNssTid.toStringWithTenantId_forTest() << "to"
                                       << targetNssTid.toStringWithTenantId_forTest());
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  renameCollectionForApplyOps(_opCtx.get(), boost::none, boost::none, cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _sourceNssTid));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), otherSourceNssTid));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), targetNssTid));
}

TEST_F(RenameCollectionTestMultitenancy, RenameCollectionForApplyOpsRequireTenantIdFalse) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);

    const NamespaceString targetNssTid =
        NamespaceString::createNamespaceString_forTest(_tenantId, _otherNs);
    ASSERT_NOT_EQUALS(_sourceNssTid, targetNssTid);

    auto cmd = BSON("renameCollection" << _sourceNssTid.toStringWithTenantId_forTest() << "to"
                                       << targetNssTid.toStringWithTenantId_forTest());
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), boost::none, boost::none, cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), targetNssTid));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNssTid));
}

TEST_F(RenameCollectionTestMultitenancy, RenameCollectionForApplyOpsSameNS) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    // A tid field supersedes tenantIds maintained in source or target. See above.
    auto cmd = BSON("renameCollection" << _sourceNssTid.toString_forTest() << "to"
                                       << _sourceNssTid.toString_forTest());
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), boost::none, _tenantId, cmd, {}));
}

TEST_F(RenameCollectionTestMultitenancy, RenameCollectionForApplyOpsSameNSRequireTenantIdFalse) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);

    auto cmd = BSON("renameCollection" << _sourceNssTid.toStringWithTenantId_forTest() << "to"
                                       << _sourceNssTid.toStringWithTenantId_forTest());
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), boost::none, boost::none, cmd, {}));
}

TEST_F(RenameCollectionTestMultitenancy, RenameCollectionForApplyOpsAcrossTenantIds) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);

    const NamespaceString targetNssTid =
        NamespaceString::createNamespaceString_forTest(_otherTenantId, _sourceNs);
    ASSERT_NOT_EQUALS(_sourceNssTid, targetNssTid);

    // This test is valid during the transition period, before featureFlagRequireTenantID is
    // enforced, and will prefix the tenantIds onto the ns fields.
    auto cmd = BSON("renameCollection" << _sourceNssTid.toStringWithTenantId_forTest() << "to"
                                       << targetNssTid.toStringWithTenantId_forTest());
    ASSERT_THROWS_CODE(renameCollectionForApplyOps(_opCtx.get(), boost::none, boost::none, cmd, {}),
                       AssertionException,
                       ErrorCodes::IllegalOperation);
}

TEST_F(RenameCollectionTestMultitenancy, RenameCollectionAcrossTenantIds) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    const NamespaceString targetNssTid =
        NamespaceString::createNamespaceString_forTest(_otherTenantId, _otherNs);
    ASSERT_NOT_EQUALS(_sourceNssTid, targetNssTid);

    // Renaming collections across tenantIds is not allowed
    RenameCollectionOptions options;
    options.dropTarget = true;
    ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                  renameCollection(_opCtx.get(), _sourceNssTid, targetNssTid, options));
}

}  // namespace
}  // namespace mongo

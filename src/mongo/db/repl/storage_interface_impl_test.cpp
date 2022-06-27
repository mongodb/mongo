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

#include "mongo/platform/basic.h"

#include <algorithm>
#include <boost/optional.hpp>
#include <memory>

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

BSONObj makeIdIndexSpec(const NamespaceString& nss) {
    return BSON("name"
                << "_id_"
                << "key" << BSON("_id" << 1) << "unique" << true << "v"
                << static_cast<int>(kIndexVersion));
}

/**
 * Generates a unique namespace from the test registration agent.
 */
template <typename T>
NamespaceString makeNamespace(const T& t, const std::string& suffix = "") {
    return NamespaceString(std::string("local." + t.getSuiteName() + "_" + t.getTestName())
                               .substr(0, NamespaceString::MaxNsCollectionLen - suffix.length()) +
                           suffix);
}

/**
 * Generates a default CollectionOptions object with a UUID. These options should be used
 * when creating a collection in this test because otherwise, collections will not be created
 * with UUIDs. All collections are expected to have UUIDs.
 */
CollectionOptions generateOptionsWithUuid() {
    CollectionOptions options;
    options.uuid = UUID::gen();
    return options;
}

CollectionOptions generateOptionsWithUuidClustered() {
    CollectionOptions options;
    options.uuid = UUID::gen();
    options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    return options;
}

/**
 * Creates collection options suitable for oplog.
 */
CollectionOptions createOplogCollectionOptions() {
    CollectionOptions options = generateOptionsWithUuid();
    options.capped = true;
    options.cappedSize = 64 * 1024 * 1024LL;
    options.autoIndexId = CollectionOptions::NO;
    return options;
}

/**
 * Create test collection.
 * Returns collection.
 */
void createCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const CollectionOptions& options = generateOptionsWithUuid()) {
    writeConflictRetry(opCtx, "createCollection", nss.ns(), [&] {
        Lock::DBLock dblk(opCtx, nss.dbName(), MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        auto db = ctx.db();
        ASSERT_TRUE(db);
        mongo::WriteUnitOfWork wuow(opCtx);
        auto coll = db->createCollection(opCtx, nss, options);
        ASSERT_TRUE(coll);
        wuow.commit();
    });
}

/**
 * Create an index on an empty collection. Returns the number of indexes that exist on the
 * collection after the given index is created.
 */
int _createIndexOnEmptyCollection(OperationContext* opCtx, NamespaceString nss, BSONObj indexSpec) {
    Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_X);
    AutoGetCollection coll(opCtx, nss, MODE_X);

    WriteUnitOfWork wunit(opCtx);
    auto indexCatalog = coll.getWritableCollection(opCtx)->getIndexCatalog();
    ASSERT(indexCatalog);

    ASSERT_OK(
        indexCatalog
            ->createIndexOnEmptyCollection(opCtx, coll.getWritableCollection(opCtx), indexSpec)
            .getStatus());
    wunit.commit();

    return indexCatalog->numIndexesReady(opCtx);
}

/**
 * Creates an oplog entry with given optime.
 */
TimestampedBSONObj makeOplogEntry(OpTime opTime) {
    BSONObjBuilder bob(opTime.toBSON());
    bob.append("op", "c");
    bob.append("ns", "test.t");
    return {bob.obj(), opTime.getTimestamp()};
}

/**
 * Counts the number of keys in an index using an IndexAccessMethod::validate call.
 */
int64_t getIndexKeyCount(OperationContext* opCtx,
                         const IndexCatalog* cat,
                         const IndexDescriptor* desc) {
    auto idx = cat->getEntry(desc)->accessMethod();
    int64_t numKeys;
    IndexValidateResults fullRes;
    idx->validate(opCtx, &numKeys, &fullRes);
    return numKeys;
}

std::vector<InsertStatement> transformInserts(std::vector<BSONObj> docs) {
    std::vector<InsertStatement> inserts(docs.size());
    std::transform(docs.cbegin(), docs.cend(), inserts.begin(), [](const BSONObj& doc) {
        return InsertStatement(doc);
    });
    return inserts;
}
std::vector<InsertStatement> transformInserts(std::vector<TimestampedBSONObj> docs) {
    std::vector<InsertStatement> inserts(docs.size());
    std::transform(docs.cbegin(), docs.cend(), inserts.begin(), [](const TimestampedBSONObj& doc) {
        return InsertStatement(doc.obj, doc.timestamp, OpTime::kUninitializedTerm);
    });
    return inserts;
}

class StorageInterfaceImplTest : public ServiceContextMongoDTest {
protected:
    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    ReplicationCoordinatorMock* getReplicationCoordinatorMock() {
        return _replicationCoordinatorMock;
    }

    void resetUnreplicatedWritesBlock() {
        _uwb.reset(nullptr);
    }

private:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _createOpCtx();
        auto service = getServiceContext();
        auto replCoord = std::make_unique<ReplicationCoordinatorMock>(service);
        _replicationCoordinatorMock = replCoord.get();
        ReplicationCoordinator::set(service, std::move(replCoord));
    }

    void tearDown() override {
        _ddv.reset(nullptr);
        _uwb.reset(nullptr);
        _opCtx.reset(nullptr);
        ServiceContextMongoDTest::tearDown();
    }

    void _createOpCtx() {
        _opCtx = cc().makeOperationContext();
        // We are not replicating nor validating these writes.
        _uwb = std::make_unique<UnreplicatedWritesBlock>(_opCtx.get());
        _ddv = std::make_unique<DisableDocumentValidation>(_opCtx.get());
    }

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<UnreplicatedWritesBlock> _uwb;
    std::unique_ptr<DisableDocumentValidation> _ddv;
    ReplicationCoordinatorMock* _replicationCoordinatorMock = nullptr;
};

TEST_F(StorageInterfaceImplTest, ServiceContextDecorator) {
    auto serviceContext = getServiceContext();
    ASSERT_FALSE(StorageInterface::get(serviceContext));
    StorageInterface* storage = new StorageInterfaceImpl();
    StorageInterface::set(serviceContext, std::unique_ptr<StorageInterface>(storage));
    ASSERT_TRUE(storage == StorageInterface::get(serviceContext));
    ASSERT_TRUE(storage == StorageInterface::get(*serviceContext));
    ASSERT_TRUE(storage == StorageInterface::get(getOperationContext()));
}

TEST_F(StorageInterfaceImplTest, GetRollbackIDReturnsNamespaceNotFoundOnMissingCollection) {
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();

    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, storage.getRollbackID(opCtx).getStatus());
}

TEST_F(StorageInterfaceImplTest, IncrementRollbackIDReturnsNamespaceNotFoundOnMissingCollection) {
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();

    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, storage.incrementRollbackID(opCtx));
}

TEST_F(StorageInterfaceImplTest, InitializeRollbackIDReturnsNamespaceExistsOnExistingCollection) {
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();

    createCollection(opCtx, NamespaceString(StorageInterfaceImpl::kDefaultRollbackIdNamespace));
    ASSERT_EQUALS(ErrorCodes::NamespaceExists, storage.initializeRollbackID(opCtx));
}

TEST_F(StorageInterfaceImplTest,
       InitializeRollbackIDReturnsNamespaceExistsIfItHasAlreadyBeenInitialized) {
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();

    ASSERT_OK(storage.initializeRollbackID(opCtx));
    ASSERT_EQUALS(ErrorCodes::NamespaceExists, storage.initializeRollbackID(opCtx));
}

/**
 * Check collection contents.
 */
void _assertDocumentsInCollectionEquals(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const std::vector<BSONObj>& docs) {
    CollectionReader reader(opCtx, nss);
    for (const auto& doc : docs) {
        ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(reader.next()));
    }
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, reader.next().getStatus());
}

void _assertDocumentsInCollectionEquals(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const std::vector<TimestampedBSONObj>& docs) {
    CollectionReader reader(opCtx, nss);
    for (const auto& doc : docs) {
        ASSERT_BSONOBJ_EQ(doc.obj, unittest::assertGet(reader.next()));
    }
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, reader.next().getStatus());
}

/**
 * Check collection contents for a singleton Rollback ID document.
 */
void _assertRollbackIDDocument(OperationContext* opCtx, int id) {
    _assertDocumentsInCollectionEquals(
        opCtx,
        NamespaceString(StorageInterfaceImpl::kDefaultRollbackIdNamespace),
        {BSON("_id" << StorageInterfaceImpl::kRollbackIdDocumentId
                    << StorageInterfaceImpl::kRollbackIdFieldName << id)});
}

TEST_F(StorageInterfaceImplTest, RollbackIdInitializesIncrementsAndReadsProperly) {
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();

    // Rollback ID should be initialized to 1.
    ASSERT_OK(storage.initializeRollbackID(opCtx));
    _assertRollbackIDDocument(opCtx, 1);

    auto rbid = unittest::assertGet(storage.getRollbackID(opCtx));
    ASSERT_EQUALS(rbid, 1);

    // Rollback ID should increment by exactly 1 each time.
    ASSERT_OK(storage.incrementRollbackID(opCtx));
    _assertRollbackIDDocument(opCtx, 2);

    rbid = unittest::assertGet(storage.getRollbackID(opCtx));
    ASSERT_EQUALS(rbid, 2);

    ASSERT_OK(storage.incrementRollbackID(opCtx));
    _assertRollbackIDDocument(opCtx, 3);

    rbid = unittest::assertGet(storage.getRollbackID(opCtx));
    ASSERT_EQUALS(rbid, 3);
}

TEST_F(StorageInterfaceImplTest, IncrementRollbackIDRollsToOneWhenExceedingMaxInt) {
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();
    NamespaceString nss(StorageInterfaceImpl::kDefaultRollbackIdNamespace);
    createCollection(opCtx, nss);
    TimestampedBSONObj maxDoc = {BSON("_id" << StorageInterfaceImpl::kRollbackIdDocumentId
                                            << StorageInterfaceImpl::kRollbackIdFieldName
                                            << std::numeric_limits<int>::max()),
                                 Timestamp::min()};
    ASSERT_OK(storage.insertDocument(opCtx, nss, maxDoc, OpTime::kUninitializedTerm));
    _assertRollbackIDDocument(opCtx, std::numeric_limits<int>::max());

    auto rbid = unittest::assertGet(storage.getRollbackID(opCtx));
    ASSERT_EQUALS(rbid, std::numeric_limits<int>::max());

    ASSERT_OK(storage.incrementRollbackID(opCtx));
    _assertRollbackIDDocument(opCtx, 1);

    rbid = unittest::assertGet(storage.getRollbackID(opCtx));
    ASSERT_EQUALS(rbid, 1);

    ASSERT_OK(storage.incrementRollbackID(opCtx));
    _assertRollbackIDDocument(opCtx, 2);

    rbid = unittest::assertGet(storage.getRollbackID(opCtx));
    ASSERT_EQUALS(rbid, 2);
}

TEST_F(StorageInterfaceImplTest, GetRollbackIDReturnsBadStatusIfDocumentHasBadField) {
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();
    NamespaceString nss(StorageInterfaceImpl::kDefaultRollbackIdNamespace);

    createCollection(opCtx, nss);

    std::vector<TimestampedBSONObj> badDocs = {
        {BSON("_id" << StorageInterfaceImpl::kRollbackIdDocumentId << "bad field" << 3),
         Timestamp::min()}};
    ASSERT_OK(storage.insertDocuments(opCtx, nss, transformInserts(badDocs)));
    ASSERT_EQUALS(ErrorCodes::duplicateCodeForTest(40415),
                  storage.getRollbackID(opCtx).getStatus());
}

TEST_F(StorageInterfaceImplTest, GetRollbackIDReturnsBadStatusIfRollbackIDIsNotInt) {
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();
    NamespaceString nss(StorageInterfaceImpl::kDefaultRollbackIdNamespace);

    createCollection(opCtx, nss);

    std::vector<TimestampedBSONObj> badDoc = {
        TimestampedBSONObj{BSON("_id" << StorageInterfaceImpl::kRollbackIdDocumentId
                                      << StorageInterfaceImpl::kRollbackIdFieldName << "bad id"),
                           Timestamp::min()}};
    ASSERT_OK(storage.insertDocuments(opCtx, nss, transformInserts(badDoc)));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, storage.getRollbackID(opCtx).getStatus());
}

TEST_F(StorageInterfaceImplTest, InsertDocumentsReturnsOKWhenNoOperationsAreGiven) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();
    createCollection(opCtx, nss, options);

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.insertDocuments(opCtx, nss, {}));
    ASSERT_OK(storage.insertDocuments(opCtx, {nss.db().toString(), *options.uuid}, {}));
}

TEST_F(StorageInterfaceImplTest,
       InsertDocumentsReturnsInternalErrorWhenSavingOperationToNonOplogCollection) {
    // Create fake non-oplog collection to ensure saving oplog entries (without _id field) will
    // fail.
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();
    createCollection(opCtx, nss, options);

    // Non-oplog collection will enforce mandatory _id field requirement on insertion.
    StorageInterfaceImpl storage;
    auto op = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto status = storage.insertDocuments(opCtx, nss, transformInserts({op}));
    ASSERT_EQUALS(ErrorCodes::InternalError, status);
    ASSERT_STRING_CONTAINS(status.reason(), "Collection::insertDocument got document without _id");

    // Again, but specify the collection with its UUID.
    ASSERT_EQ(ErrorCodes::InternalError,
              storage.insertDocuments(
                  opCtx, {nss.db().toString(), *options.uuid}, transformInserts({op})));
}

TEST_F(StorageInterfaceImplTest,
       InsertDocumentsInsertsDocumentsOneAtATimeWhenAllAtOnceInsertingFails) {
    // Create a collection that does not support all-at-once inserting.
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    CollectionOptions options = generateOptionsWithUuid();
    options.capped = true;
    options.cappedSize = 1024 * 1024;
    createCollection(opCtx, nss, options);
    // StorageInterfaceImpl::insertDocuments should fall back on inserting the batch one at a time.
    StorageInterfaceImpl storage;
    auto doc1 = InsertStatement(BSON("_id" << 1));
    auto doc2 = InsertStatement(BSON("_id" << 2));
    std::vector<InsertStatement> docs{doc1, doc2};
    // Confirm that Collection::insertDocuments fails to insert the batch all at once.
    {
        AutoGetCollection autoCollection(opCtx, nss, MODE_IX);
        WriteUnitOfWork wunit(opCtx);
        ASSERT_EQUALS(ErrorCodes::OperationCannotBeBatched,
                      autoCollection.getCollection()->insertDocuments(
                          opCtx, docs.cbegin(), docs.cend(), nullptr, false));
    }
    ASSERT_OK(storage.insertDocuments(opCtx, nss, docs));

    // Check collection contents.
    _assertDocumentsInCollectionEquals(opCtx, nss, {doc1.doc, doc2.doc});
}

TEST_F(StorageInterfaceImplTest, InsertDocumentsSavesOperationsReturnsOpTimeOfLastOperation) {
    // Create fake oplog collection to hold operations.
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss, createOplogCollectionOptions());

    // Insert operations using storage interface. Ensure optime return is consistent with last
    // operation inserted.
    StorageInterfaceImpl storage;
    auto op1 = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto op2 = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    ASSERT_OK(storage.insertDocuments(opCtx, nss, transformInserts({op1, op2})));

    // Check contents of oplog.
    _assertDocumentsInCollectionEquals(opCtx, nss, {op1.obj, op2.obj});
}

TEST_F(StorageInterfaceImplTest, InsertDocumentsSavesOperationsWhenCollSpecifiedWithUUID) {
    // This is exactly like the test InsertDocumentsSavesOperationsReturnsOpTimeOfLastOperation, but
    // with the UUID specified instead of the namespace string.
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = createOplogCollectionOptions();
    createCollection(opCtx, nss, options);

    // Insert operations using storage interface. Ensure optime return is consistent with last
    // operation inserted.
    StorageInterfaceImpl storage;
    auto op1 = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto op2 = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    ASSERT_OK(storage.insertDocuments(
        opCtx, {nss.db().toString(), *options.uuid}, transformInserts({op1, op2})));

    // Check contents of oplog.
    _assertDocumentsInCollectionEquals(opCtx, nss, {op1.obj, op2.obj});
}

TEST_F(StorageInterfaceImplTest,
       InsertDocumentsReturnsNamespaceNotFoundIfOplogCollectionDoesNotExist) {
    auto op = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto nss = makeNamespace(_agent);
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();
    auto status = storage.insertDocuments(opCtx, nss, transformInserts({op}));
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
    ASSERT_STRING_CONTAINS(status.reason(), "The collection must exist before inserting documents");
}

TEST_F(StorageInterfaceImplTest, InsertDocumentThrowsNamespaceNotFoundIfOplogUUIDNotInCatalog) {
    auto op = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    ASSERT_THROWS_CODE(
        storage.insertDocuments(opCtx, {"local", UUID::gen()}, transformInserts({op})),
        DBException,
        ErrorCodes::NamespaceNotFound);
}

TEST_F(StorageInterfaceImplTest, InsertMissingDocWorksOnExistingCappedCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    CollectionOptions opts = generateOptionsWithUuid();
    opts.capped = true;
    opts.cappedSize = 1024 * 1024;
    createCollection(opCtx, nss, opts);
    ASSERT_OK(storage.insertDocument(
        opCtx, nss, {BSON("_id" << 1), Timestamp(1)}, OpTime::kUninitializedTerm));
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_TRUE(autoColl.getCollection());
}

TEST_F(StorageInterfaceImplTest, InsertDocWorksWithExistingCappedCollectionSpecifiedByUUID) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();
    options.capped = true;
    options.cappedSize = 1024 * 1024;
    createCollection(opCtx, nss, options);

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.insertDocument(opCtx,
                                     {nss.db().toString(), *options.uuid},
                                     {BSON("_id" << 1), Timestamp(1)},
                                     OpTime::kUninitializedTerm));
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_TRUE(autoColl.getCollection());
}

TEST_F(StorageInterfaceImplTest, InsertMissingDocWorksOnExistingCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss);
    ASSERT_OK(storage.insertDocument(
        opCtx, nss, {BSON("_id" << 1), Timestamp(1)}, OpTime::kUninitializedTerm));
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_TRUE(autoColl.getCollection());
}

TEST_F(StorageInterfaceImplTest, InsertMissingDocFailesIfCollectionIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    const auto status = storage.insertDocument(
        opCtx, nss, {BSON("_id" << 1), Timestamp(1)}, OpTime::kUninitializedTerm);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::NamespaceNotFound);
}

TEST_F(StorageInterfaceImplTest, CreateCollectionWithIDIndexCommits) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    CollectionOptions opts = generateOptionsWithUuid();
    std::vector<BSONObj> indexes;
    auto loaderStatus =
        storage.createCollectionForBulkLoading(nss, opts, makeIdIndexSpec(nss), indexes);
    ASSERT_OK(loaderStatus.getStatus());
    auto loader = std::move(loaderStatus.getValue());
    std::vector<BSONObj> docs = {BSON("_id" << 1), BSON("_id" << 1), BSON("_id" << 2)};
    ASSERT_OK(loader->insertDocuments(docs.begin(), docs.end()));
    ASSERT_OK(loader->commit());

    AutoGetCollectionForReadCommand coll(opCtx, nss);
    ASSERT(coll);
    ASSERT_EQ(coll->getRecordStore()->numRecords(opCtx), 2LL);
    auto collIdxCat = coll->getIndexCatalog();
    auto idIdxDesc = collIdxCat->findIdIndex(opCtx);
    auto count = getIndexKeyCount(opCtx, collIdxCat, idIdxDesc);
    ASSERT_EQ(count, 2LL);
}

void _testDestroyUncommitedCollectionBulkLoader(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::vector<BSONObj> secondaryIndexes,
    std::function<void(std::unique_ptr<CollectionBulkLoader> loader)> destroyLoaderFn) {
    StorageInterfaceImpl storage;
    CollectionOptions opts = generateOptionsWithUuid();
    auto loaderStatus =
        storage.createCollectionForBulkLoading(nss, opts, makeIdIndexSpec(nss), secondaryIndexes);
    ASSERT_OK(loaderStatus.getStatus());
    auto loader = std::move(loaderStatus.getValue());
    std::vector<BSONObj> docs = {BSON("_id" << 1)};
    ASSERT_OK(loader->insertDocuments(docs.begin(), docs.end()));

    // Destroy bulk loader.
    // Collection and ID index should not exist after 'loader' is destroyed.
    destroyLoaderFn(std::move(loader));

    AutoGetCollectionForReadCommand coll(opCtx, nss);

    // Bulk loader is used to create indexes. The collection is not dropped when the bulk loader is
    // destroyed.
    ASSERT_TRUE(coll);
    ASSERT_EQ(1LL, coll->getRecordStore()->numRecords(opCtx));

    // IndexCatalog::numIndexesTotal() includes unfinished indexes. We need to ensure that
    // the bulk loader drops the unfinished indexes.
    auto collIdxCat = coll->getIndexCatalog();
    ASSERT_EQUALS(0, collIdxCat->numIndexesTotal(opCtx));
}

TEST_F(StorageInterfaceImplTest, DestroyingUncommittedCollectionBulkLoaderDropsIndexes) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    std::vector<BSONObj> indexes = {BSON("v" << 1 << "key" << BSON("x" << 1) << "name"
                                             << "x_1")};
    auto destroyLoaderFn = [](std::unique_ptr<CollectionBulkLoader> loader) {
        // Destroy 'loader' by letting it go out of scope.
    };
    _testDestroyUncommitedCollectionBulkLoader(opCtx, nss, indexes, destroyLoaderFn);
}

TEST_F(StorageInterfaceImplTest, DestructorInitializesClientBeforeDestroyingIdIndexBuilder) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    std::vector<BSONObj> indexes;
    auto destroyLoaderFn = [](std::unique_ptr<CollectionBulkLoader> loader) {
        // Destroy 'loader' in a new thread that does not have a Client.
        stdx::thread([&loader]() { loader.reset(); }).join();
    };
    _testDestroyUncommitedCollectionBulkLoader(opCtx, nss, indexes, destroyLoaderFn);
}

TEST_F(StorageInterfaceImplTest,
       DestructorInitializesClientBeforeDestroyingSecondaryIndexesBuilder) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    std::vector<BSONObj> indexes = {BSON("v" << 1 << "key" << BSON("x" << 1) << "name"
                                             << "x_1")};
    auto destroyLoaderFn = [](std::unique_ptr<CollectionBulkLoader> loader) {
        // Destroy 'loader' in a new thread that does not have a Client.
        stdx::thread([&loader]() { loader.reset(); }).join();
    };
    _testDestroyUncommitedCollectionBulkLoader(opCtx, nss, indexes, destroyLoaderFn);
}

TEST_F(StorageInterfaceImplTest, CreateCollectionThatAlreadyExistsFails) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("test.foo");
    createCollection(opCtx, nss);

    const CollectionOptions opts = generateOptionsWithUuid();
    const std::vector<BSONObj> indexes;
    const auto status =
        storage.createCollectionForBulkLoading(nss, opts, makeIdIndexSpec(nss), indexes);
    ASSERT_NOT_OK(status.getStatus());
}

TEST_F(StorageInterfaceImplTest, CreateOplogCreateCappedCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("local.oplog.X");
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_FALSE(autoColl.getCollection());
    }
    ASSERT_OK(storage.createOplog(opCtx, nss));
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_TRUE(autoColl.getCollection());
        ASSERT_EQ(nss.toString(), autoColl.getCollection()->ns().toString());
        ASSERT_TRUE(autoColl.getCollection()->isCapped());
    }
}

TEST_F(StorageInterfaceImplTest,
       CreateCollectionReturnsUserExceptionAsStatusIfCollectionCreationThrows) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("local.oplog.Y");
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_FALSE(autoColl.getCollection());
    }

    auto status = storage.createCollection(opCtx, nss, generateOptionsWithUuid());
    ASSERT_EQUALS(ErrorCodes::duplicateCodeForTest(28838), status);
    ASSERT_STRING_CONTAINS(status.reason(), "cannot create a non-capped oplog collection");
}

TEST_F(StorageInterfaceImplTest, CreateCollectionFailsIfCollectionExists) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_FALSE(autoColl.getCollection());
    }
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_TRUE(autoColl.getCollection());
        ASSERT_EQ(nss.toString(), autoColl.getCollection()->ns().toString());
    }
    auto status = storage.createCollection(opCtx, nss, generateOptionsWithUuid());
    ASSERT_EQUALS(ErrorCodes::NamespaceExists, status);
    ASSERT_STRING_CONTAINS(status.reason(),
                           str::stream() << "Collection " << nss.ns() << " already exists");
}

TEST_F(StorageInterfaceImplTest, DropCollectionWorksWithExistingWithDataCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss);
    ASSERT_OK(storage.insertDocument(
        opCtx, nss, {BSON("_id" << 1), Timestamp(1)}, OpTime::kUninitializedTerm));
    ASSERT_OK(storage.dropCollection(opCtx, nss));
}

TEST_F(StorageInterfaceImplTest, DropCollectionWorksWithExistingEmptyCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss);
    ASSERT_OK(storage.dropCollection(opCtx, nss));
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_FALSE(autoColl.getCollection());
}

TEST_F(StorageInterfaceImplTest, DropCollectionWorksWithMissingCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_FALSE(AutoGetDb(opCtx, nss.db(), MODE_IS).getDb());
    ASSERT_OK(storage.dropCollection(opCtx, nss));
    ASSERT_FALSE(AutoGetCollectionForReadCommand(opCtx, nss).getCollection());
    // Database should not be created after running dropCollection.
    ASSERT_FALSE(AutoGetDb(opCtx, nss.db(), MODE_IS).getDb());
}

TEST_F(StorageInterfaceImplTest, DropCollectionWorksWithSystemCollection) {
    NamespaceString nss("local.system.mysyscoll");
    ASSERT_TRUE(nss.isSystem());

    // If we can create a system collection using the StorageInterface, we should be able to drop it
    // using the same interface.
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;

    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_TRUE(AutoGetCollectionForReadCommand(opCtx, nss).getCollection());

    ASSERT_OK(storage.dropCollection(opCtx, nss));
    ASSERT_FALSE(AutoGetCollectionForReadCommand(opCtx, nss).getCollection());
}

TEST_F(StorageInterfaceImplTest, RenameCollectionWorksWhenCollectionExists) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto toNss = NamespaceString("local.toNs");
    createCollection(opCtx, nss);

    ASSERT_OK(storage.renameCollection(opCtx, nss, toNss, false));

    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_FALSE(autoColl.getCollection());

    AutoGetCollectionForReadCommand autoColl2(opCtx, toNss);
    ASSERT_TRUE(autoColl2.getCollection());
}

TEST_F(StorageInterfaceImplTest, RenameCollectionWithStayTempFalseMakesItNotTemp) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto toNss = NamespaceString("local.toNs");
    CollectionOptions opts = generateOptionsWithUuid();
    opts.temp = true;
    createCollection(opCtx, nss, opts);

    ASSERT_OK(storage.renameCollection(opCtx, nss, toNss, false));

    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_FALSE(autoColl.getCollection());

    AutoGetCollectionForReadCommand autoColl2(opCtx, toNss);
    ASSERT_TRUE(autoColl2.getCollection());
    ASSERT_FALSE(autoColl2->getCollectionOptions().temp);
}

TEST_F(StorageInterfaceImplTest, RenameCollectionWithStayTempTrueMakesItTemp) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto toNss = NamespaceString("local.toNs");
    CollectionOptions opts = generateOptionsWithUuid();
    opts.temp = true;
    createCollection(opCtx, nss, opts);

    ASSERT_OK(storage.renameCollection(opCtx, nss, toNss, true));

    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_FALSE(autoColl.getCollection());

    AutoGetCollectionForReadCommand autoColl2(opCtx, toNss);
    ASSERT_TRUE(autoColl2.getCollection());
    ASSERT_TRUE(autoColl2->getCollectionOptions().temp);
}

TEST_F(StorageInterfaceImplTest, RenameCollectionFailsBetweenDatabases) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto toNss = NamespaceString("notLocal.toNs");
    createCollection(opCtx, nss);

    ASSERT_EQ(ErrorCodes::InvalidNamespace, storage.renameCollection(opCtx, nss, toNss, false));

    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_TRUE(autoColl.getCollection());

    AutoGetCollectionForReadCommand autoColl2(opCtx, toNss);
    ASSERT_FALSE(autoColl2.getCollection());
}

TEST_F(StorageInterfaceImplTest, RenameCollectionFailsWhenToCollectionAlreadyExists) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto toNss = NamespaceString("local.toNs");
    createCollection(opCtx, nss);
    createCollection(opCtx, toNss);

    ASSERT_EQ(ErrorCodes::NamespaceExists, storage.renameCollection(opCtx, nss, toNss, false));

    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_TRUE(autoColl.getCollection());

    AutoGetCollectionForReadCommand autoColl2(opCtx, toNss);
    ASSERT_TRUE(autoColl2.getCollection());
}

TEST_F(StorageInterfaceImplTest, RenameCollectionFailsWhenFromCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto toNss = NamespaceString("local.toNs");

    ASSERT_EQ(ErrorCodes::NamespaceNotFound, storage.renameCollection(opCtx, nss, toNss, false));

    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_FALSE(autoColl.getCollection());

    AutoGetCollectionForReadCommand autoColl2(opCtx, toNss);
    ASSERT_FALSE(autoColl2.getCollection());
}

TEST_F(StorageInterfaceImplTest, FindDocumentsReturnsInvalidNamespaceIfCollectionIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     indexName,
                                     StorageInterface::ScanDirection::kForward,
                                     {},
                                     BoundInclusion::kIncludeStartKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, FindDocumentsReturnsIndexNotFoundIfIndexIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "nonexistent"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_EQUALS(ErrorCodes::IndexNotFound,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     indexName,
                                     StorageInterface::ScanDirection::kForward,
                                     {},
                                     BoundInclusion::kIncludeStartKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, FindDocumentsReturnsIndexOptionsConflictIfIndexIsAPartialIndex) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    std::vector<BSONObj> indexes = {BSON("v" << 1 << "key" << BSON("x" << 1) << "name"
                                             << "x_1"
                                             << "partialFilterExpression" << BSON("y" << 1))};
    auto loader = unittest::assertGet(storage.createCollectionForBulkLoading(
        nss, generateOptionsWithUuid(), makeIdIndexSpec(nss), indexes));
    std::vector<BSONObj> docs = {BSON("_id" << 1), BSON("_id" << 1), BSON("_id" << 2)};
    ASSERT_OK(loader->insertDocuments(docs.begin(), docs.end()));
    ASSERT_OK(loader->commit());
    auto indexName = "x_1"_sd;
    ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     indexName,
                                     StorageInterface::ScanDirection::kForward,
                                     {},
                                     BoundInclusion::kIncludeStartKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, FindDocumentsReturnsEmptyVectorIfCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_TRUE(unittest::assertGet(storage.findDocuments(opCtx,
                                                          nss,
                                                          indexName,
                                                          StorageInterface::ScanDirection::kForward,
                                                          {},
                                                          BoundInclusion::kIncludeStartKeyOnly,
                                                          1U))
                    .empty());
}

std::string _toString(const std::vector<BSONObj>& docs) {
    str::stream ss;
    ss << "[";
    bool first = true;
    for (const auto& doc : docs) {
        if (first) {
            ss << doc;
            first = false;
        } else {
            ss << ", " << doc;
        }
    }
    ss << "]";
    return ss;
}

/**
 * Check StatusWith<std::vector<BSONObj>> value.
 */
void _assertDocumentsEqual(const StatusWith<std::vector<BSONObj>>& statusWithDocs,
                           const std::vector<BSONObj>& expectedDocs) {
    const auto actualDocs = unittest::assertGet(statusWithDocs);
    auto iter = actualDocs.cbegin();
    std::string msg = str::stream()
        << "expected: " << _toString(expectedDocs) << "; actual: " << _toString(actualDocs);
    for (const auto& doc : expectedDocs) {
        ASSERT_TRUE(iter != actualDocs.cend()) << msg;
        ASSERT_BSONOBJ_EQ(doc, *(iter++));
    }
    ASSERT_TRUE(iter == actualDocs.cend()) << msg;
}

/**
 * Returns first BSONObj from a StatusWith<std::vector<BSONObj>>.
 */
BSONObj _assertGetFront(const StatusWith<std::vector<BSONObj>>& statusWithDocs) {
    auto&& docs = statusWithDocs.getValue();
    ASSERT_FALSE(docs.empty());
    return docs.front();
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsReturnsDocumentWithLowestKeyValueIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(storage.insertDocuments(
        opCtx,
        nss,
        transformInserts(std::vector<TimestampedBSONObj>{{BSON("_id" << 0), Timestamp(0)},
                                                         {BSON("_id" << 1), Timestamp(1)},
                                                         {BSON("_id" << 2), Timestamp(2)},
                                                         {BSON("_id" << 3), Timestamp(3)},
                                                         {BSON("_id" << 4), Timestamp(4)}})));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              {},
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                0U),
                          {});

    // startKey not provided. limit of 2.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                2U),
                          {BSON("_id" << 0), BSON("_id" << 1)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 0),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 0.5),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey provided; include both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1),
                                              BoundInclusion::kIncludeEndKeyOnly,
                                              1U)));

    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1.5),
                                              BoundInclusion::kIncludeEndKeyOnly,
                                              1U)));

    // startKey provided; exclude both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1),
                                              BoundInclusion::kExcludeBothStartAndEndKeys,
                                              1U)));

    // startKey provided; exclude both start and end keys.
    // A limit of 3 should return 2 documents because we reached the end of the collection.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                BSON("" << 2),
                                                BoundInclusion::kExcludeBothStartAndEndKeys,
                                                3U),
                          {BSON("_id" << 3), BSON("_id" << 4)});

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3), BSON("_id" << 4)});
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsReturnsDocumentWithHighestKeyValueIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 3), Timestamp(3), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 4), Timestamp(4), OpTime::kUninitializedTerm}}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 4),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              {},
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                0U),
                          {});

    // startKey not provided. limit of 2.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                2U),
                          {BSON("_id" << 4), BSON("_id" << 3)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 4),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 4),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 3),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 3),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey provided; include both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 4),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 4),
                                              BoundInclusion::kIncludeBothStartAndEndKeys,
                                              1U)));

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 3),
                                              BoundInclusion::kIncludeEndKeyOnly,
                                              1U)));

    // startKey provided; exclude both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 3),
                                              BoundInclusion::kExcludeBothStartAndEndKeys,
                                              1U)));

    // startKey provided; exclude both start and end keys.
    // A limit of 3 should return 2 documents because we reached the beginning of the collection.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                BSON("" << 2),
                                                BoundInclusion::kExcludeBothStartAndEndKeys,
                                                3U),
                          {BSON("_id" << 1), BSON("_id" << 0)});

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3), BSON("_id" << 4)});
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsCollScanReturnsFirstDocumentInsertedIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              boost::none,
                                              StorageInterface::ScanDirection::kForward,
                                              {},
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // Check collection contents.
    _assertDocumentsInCollectionEquals(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)});
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsCollScanReturnsLastDocumentInsertedIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              boost::none,
                                              StorageInterface::ScanDirection::kBackward,
                                              {},
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    _assertDocumentsInCollectionEquals(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)});
}

TEST_F(StorageInterfaceImplTest, FindDocumentsCollScanReturnsNoSuchKeyIfStartKeyIsNotEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     boost::none,
                                     StorageInterface::ScanDirection::kForward,
                                     BSON("" << 1),
                                     BoundInclusion::kIncludeStartKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsCollScanReturnsInvalidOptionsIfBoundIsNotStartKeyOnly) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::InvalidOptions,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     boost::none,
                                     StorageInterface::ScanDirection::kForward,
                                     {},
                                     BoundInclusion::kIncludeEndKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, DeleteDocumentsReturnsInvalidNamespaceIfCollectionIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       indexName,
                                       StorageInterface::ScanDirection::kForward,
                                       {},
                                       BoundInclusion::kIncludeStartKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, DeleteDocumentsReturnsIndexNotFoundIfIndexIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "nonexistent"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_EQUALS(ErrorCodes::IndexNotFound,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       indexName,
                                       StorageInterface::ScanDirection::kForward,
                                       {},
                                       BoundInclusion::kIncludeStartKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, DeleteDocumentsReturnsEmptyVectorIfCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_TRUE(
        unittest::assertGet(storage.deleteDocuments(opCtx,
                                                    nss,
                                                    indexName,
                                                    StorageInterface::ScanDirection::kForward,
                                                    {},
                                                    BoundInclusion::kIncludeStartKeyOnly,
                                                    1U))
            .empty());
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsReturnsDocumentWithLowestKeyValueIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 3), Timestamp(3), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 4), Timestamp(4), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 5), Timestamp(5), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 6), Timestamp(6), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 7), Timestamp(7), OpTime::kUninitializedTerm}}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6),
                                        BSON("_id" << 7)});

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kForward,
                                                  {},
                                                  BoundInclusion::kIncludeStartKeyOnly,
                                                  0U),
                          {});

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6),
                                        BSON("_id" << 7)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                BSON("" << 2),
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 1),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6),
                                        BSON("_id" << 7)});

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 5),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                BSON("" << 4),
                                                BoundInclusion::kIncludeEndKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 4), BSON("_id" << 6), BSON("_id" << 7)});

    // startKey provided; exclude start key.
    // A limit of 3 should return 2 documents because we reached the end of the collection.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kForward,
                                                  BSON("" << 4),
                                                  BoundInclusion::kIncludeEndKeyOnly,
                                                  3U),
                          {BSON("_id" << 6), BSON("_id" << 7)});

    _assertDocumentsInCollectionEquals(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 4)});
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsReturnsDocumentWithHighestKeyValueIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 3), Timestamp(3), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 4), Timestamp(4), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 5), Timestamp(5), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 6), Timestamp(6), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 7), Timestamp(7), OpTime::kUninitializedTerm}}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 7),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0),
                                        BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6)});

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kBackward,
                                                  {},
                                                  BoundInclusion::kIncludeStartKeyOnly,
                                                  0U),
                          {});

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0),
                                        BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 5),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                BSON("" << 5),
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0),
                                        BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 6)});

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                BSON("" << 3),
                                                BoundInclusion::kIncludeEndKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 4), BSON("_id" << 6)});

    // startKey provided; exclude start key.
    // A limit of 3 should return 2 documents because we reached the beginning of the collection.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kBackward,
                                                  BSON("" << 3),
                                                  BoundInclusion::kIncludeEndKeyOnly,
                                                  3U),
                          {BSON("_id" << 1), BSON("_id" << 0)});

    _assertDocumentsInCollectionEquals(
        opCtx, nss, {BSON("_id" << 3), BSON("_id" << 4), BSON("_id" << 6)});
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsCollScanReturnsFirstDocumentInsertedIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                boost::none,
                                                StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx, nss, {BSON("_id" << 2), BSON("_id" << 0)});
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsCollScanReturnsLastDocumentInsertedIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                boost::none,
                                                StorageInterface::ScanDirection::kBackward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2)});
}

TEST_F(StorageInterfaceImplTest, DeleteDocumentsCollScanReturnsNoSuchKeyIfStartKeyIsNotEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       boost::none,
                                       StorageInterface::ScanDirection::kForward,
                                       BSON("" << 1),
                                       BoundInclusion::kIncludeStartKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsCollScanReturnsInvalidOptionsIfBoundIsNotStartKeyOnly) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::InvalidOptions,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       boost::none,
                                       StorageInterface::ScanDirection::kForward,
                                       {},
                                       BoundInclusion::kIncludeEndKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest,
       ClusteredFindDocumentsReturnsDocumentWithLowestKeyValueIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_OK(storage.insertDocuments(
        opCtx,
        nss,
        transformInserts(std::vector<TimestampedBSONObj>{{BSON("_id" << 0), Timestamp(0)},
                                                         {BSON("_id" << 1), Timestamp(1)},
                                                         {BSON("_id" << 2), Timestamp(2)},
                                                         {BSON("_id" << 3), Timestamp(3)},
                                                         {BSON("_id" << 4), Timestamp(4)}})));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              {},
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                0U),
                          {});

    // startKey not provided. limit of 2.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                2U),
                          {BSON("_id" << 0), BSON("_id" << 1)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 0),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 0.5),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey provided; include both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1),
                                              BoundInclusion::kIncludeEndKeyOnly,
                                              1U)));

    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1.5),
                                              BoundInclusion::kIncludeEndKeyOnly,
                                              1U)));

    // startKey provided; exclude both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kForward,
                                              BSON("" << 1),
                                              BoundInclusion::kExcludeBothStartAndEndKeys,
                                              1U)));

    // startKey provided; exclude both start and end keys.
    // A limit of 3 should return 2 documents because we reached the end of the collection.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                BSON("" << 2),
                                                BoundInclusion::kExcludeBothStartAndEndKeys,
                                                3U),
                          {BSON("_id" << 3), BSON("_id" << 4)});

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3), BSON("_id" << 4)});
}

TEST_F(StorageInterfaceImplTest,
       ClusteredFindDocumentsReturnsDocumentWithHighestKeyValueIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 3), Timestamp(3), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 4), Timestamp(4), OpTime::kUninitializedTerm}}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 4),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              {},
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                0U),
                          {});

    // startKey not provided. limit of 2.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                2U),
                          {BSON("_id" << 4), BSON("_id" << 3)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 4),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 4),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 3),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 3),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              1U)));

    // startKey provided; include both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 4),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 4),
                                              BoundInclusion::kIncludeBothStartAndEndKeys,
                                              1U)));

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 3),
                                              BoundInclusion::kIncludeEndKeyOnly,
                                              1U)));

    // startKey provided; exclude both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.findDocuments(opCtx,
                                              nss,
                                              indexName,
                                              StorageInterface::ScanDirection::kBackward,
                                              BSON("" << 3),
                                              BoundInclusion::kExcludeBothStartAndEndKeys,
                                              1U)));

    // startKey provided; exclude both start and end keys.
    // A limit of 3 should return 2 documents because we reached the beginning of the collection.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                BSON("" << 2),
                                                BoundInclusion::kExcludeBothStartAndEndKeys,
                                                3U),
                          {BSON("_id" << 1), BSON("_id" << 0)});

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3), BSON("_id" << 4)});
}

TEST_F(StorageInterfaceImplTest,
       ClusteredFindDocumentsCollScanReturnsNoSuchKeyIfStartKeyIsNotEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     boost::none,
                                     StorageInterface::ScanDirection::kForward,
                                     BSON("" << 1),
                                     BoundInclusion::kIncludeStartKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest,
       ClusteredFindDocumentsCollScanReturnsInvalidOptionsIfBoundIsNotStartKeyOnly) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::InvalidOptions,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     boost::none,
                                     StorageInterface::ScanDirection::kForward,
                                     {},
                                     BoundInclusion::kIncludeEndKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest,
       ClusteredDeleteDocumentsReturnsInvalidNamespaceIfCollectionIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       indexName,
                                       StorageInterface::ScanDirection::kForward,
                                       {},
                                       BoundInclusion::kIncludeStartKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, ClusteredDeleteDocumentsReturnsIndexNotFoundIfIndexIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "nonexistent"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_EQUALS(ErrorCodes::IndexNotFound,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       indexName,
                                       StorageInterface::ScanDirection::kForward,
                                       {},
                                       BoundInclusion::kIncludeStartKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, ClusteredDeleteDocumentsReturnsEmptyVectorIfCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_TRUE(
        unittest::assertGet(storage.deleteDocuments(opCtx,
                                                    nss,
                                                    indexName,
                                                    StorageInterface::ScanDirection::kForward,
                                                    {},
                                                    BoundInclusion::kIncludeStartKeyOnly,
                                                    1U))
            .empty());
}

TEST_F(StorageInterfaceImplTest,
       ClusteredDeleteDocumentsReturnsDocumentWithLowestKeyValueIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 3), Timestamp(3), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 4), Timestamp(4), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 5), Timestamp(5), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 6), Timestamp(6), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 7), Timestamp(7), OpTime::kUninitializedTerm}}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6),
                                        BSON("_id" << 7)});

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kForward,
                                                  {},
                                                  BoundInclusion::kIncludeStartKeyOnly,
                                                  0U),
                          {});

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6),
                                        BSON("_id" << 7)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                BSON("" << 2),
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 1),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6),
                                        BSON("_id" << 7)});

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 5),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                BSON("" << 4),
                                                BoundInclusion::kIncludeEndKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 4), BSON("_id" << 6), BSON("_id" << 7)});

    // startKey provided; exclude start key.
    // A limit of 3 should return 2 documents because we reached the end of the collection.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kForward,
                                                  BSON("" << 4),
                                                  BoundInclusion::kIncludeEndKeyOnly,
                                                  3U),
                          {BSON("_id" << 6), BSON("_id" << 7)});

    _assertDocumentsInCollectionEquals(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 4)});
}

TEST_F(StorageInterfaceImplTest,
       ClusteredDeleteDocumentsReturnsDocumentWithHighestKeyValueIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 1), Timestamp(1), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(2), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 3), Timestamp(3), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 4), Timestamp(4), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 5), Timestamp(5), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 6), Timestamp(6), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 7), Timestamp(7), OpTime::kUninitializedTerm}}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 7),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0),
                                        BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6)});

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kBackward,
                                                  {},
                                                  BoundInclusion::kIncludeStartKeyOnly,
                                                  0U),
                          {});

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0),
                                        BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 5),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                BSON("" << 5),
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0),
                                        BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 6)});

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assertGetFront(storage.deleteDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                BSON("" << 3),
                                                BoundInclusion::kIncludeEndKeyOnly,
                                                1U)));

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 4), BSON("_id" << 6)});

    // startKey provided; exclude start key.
    // A limit of 3 should return 2 documents because we reached the beginning of the collection.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kBackward,
                                                  BSON("" << 3),
                                                  BoundInclusion::kIncludeEndKeyOnly,
                                                  3U),
                          {BSON("_id" << 1), BSON("_id" << 0)});

    _assertDocumentsInCollectionEquals(
        opCtx, nss, {BSON("_id" << 3), BSON("_id" << 4), BSON("_id" << 6)});
}

TEST_F(StorageInterfaceImplTest,
       ClusteredDeleteDocumentsCollScanReturnsNoSuchKeyIfStartKeyIsNotEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       boost::none,
                                       StorageInterface::ScanDirection::kForward,
                                       BSON("" << 1),
                                       BoundInclusion::kIncludeStartKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest,
       ClusteredDeleteDocumentsCollScanReturnsInvalidOptionsIfBoundIsNotStartKeyOnly) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuidClustered()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::InvalidOptions,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       boost::none,
                                       StorageInterface::ScanDirection::kForward,
                                       {},
                                       BoundInclusion::kIncludeEndKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, FindSingletonReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("nosuchdb.coll");
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, storage.findSingleton(opCtx, nss).getStatus());
}

TEST_F(StorageInterfaceImplTest, FindSingletonReturnsNamespaceNotFoundWhenCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("db.coll1");
    ASSERT_OK(
        storage.createCollection(opCtx, NamespaceString("db.coll2"), generateOptionsWithUuid()));
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, storage.findSingleton(opCtx, nss).getStatus());
}

TEST_F(StorageInterfaceImplTest, FindSingletonReturnsCollectionIsEmptyWhenCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, storage.findSingleton(opCtx, nss).getStatus());
}

TEST_F(StorageInterfaceImplTest,
       FindSingletonReturnsTooManyMatchingDocumentsWhenNotSingletonCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    auto doc2 = BSON("_id" << 1 << "x" << 1);
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {{doc1, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc2, Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::TooManyMatchingDocuments,
                  storage.findSingleton(opCtx, nss).getStatus());
}

TEST_F(StorageInterfaceImplTest, FindSingletonReturnsDocumentWhenSingletonDocumentExists) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    ASSERT_OK(storage.insertDocument(opCtx, nss, {doc1, Timestamp(0)}, OpTime::kUninitializedTerm));
    ASSERT_BSONOBJ_EQ(doc1, unittest::assertGet(storage.findSingleton(opCtx, nss)));
}

TEST_F(StorageInterfaceImplTest, PutSingletonReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("nosuchdb.coll");

    TimestampedBSONObj update;
    update.obj = BSON("$set" << BSON("_id" << 0 << "x" << 1));
    update.timestamp = Timestamp();

    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, storage.putSingleton(opCtx, nss, update));
}

TEST_F(StorageInterfaceImplTest, PutSingletonReturnsNamespaceNotFoundWhenCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("db.coll1");
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    TimestampedBSONObj update;
    update.obj = BSON("$set" << BSON("_id" << 0 << "x" << 1));
    update.timestamp = Timestamp();

    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage.putSingleton(opCtx, NamespaceString("db.coll2"), update));
}

TEST_F(StorageInterfaceImplTest, PutSingletonUpsertsDocumentsWhenCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    TimestampedBSONObj update;
    update.obj = BSON("$set" << BSON("_id" << 0 << "x" << 1));
    update.timestamp = Timestamp();

    ASSERT_OK(storage.putSingleton(opCtx, nss, update));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 0 << "x" << 1),
                      unittest::assertGet(storage.findSingleton(opCtx, nss)));
    _assertDocumentsInCollectionEquals(opCtx, nss, {BSON("_id" << 0 << "x" << 1)});
}

TEST_F(StorageInterfaceImplTest, PutSingletonUpdatesDocumentWhenCollectionIsNotEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    ASSERT_OK(storage.insertDocument(opCtx, nss, {doc1, Timestamp(0)}, OpTime::kUninitializedTerm));

    TimestampedBSONObj update;
    update.obj = BSON("$set" << BSON("x" << 1));
    update.timestamp = Timestamp();

    ASSERT_OK(storage.putSingleton(opCtx, nss, update));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 0 << "x" << 1),
                      unittest::assertGet(storage.findSingleton(opCtx, nss)));
    _assertDocumentsInCollectionEquals(opCtx, nss, {BSON("_id" << 0 << "x" << 1)});
}

TEST_F(StorageInterfaceImplTest, PutSingletonUpdatesFirstDocumentWhenCollectionIsNotSingleton) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    auto doc2 = BSON("_id" << 1 << "x" << 1);
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {{doc1, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc2, Timestamp(0), OpTime::kUninitializedTerm}}));

    TimestampedBSONObj update;
    update.obj = BSON("$set" << BSON("x" << 2));
    update.timestamp = Timestamp();

    ASSERT_OK(storage.putSingleton(opCtx, nss, update));
    _assertDocumentsInCollectionEquals(opCtx, nss, {BSON("_id" << 0 << "x" << 2), doc2});
}

TEST_F(StorageInterfaceImplTest, UpdateSingletonNeverUpserts) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    TimestampedBSONObj update;
    update.obj = BSON("$set" << BSON("_id" << 0 << "x" << 1));
    update.timestamp = Timestamp();

    ASSERT_OK(storage.updateSingleton(opCtx, nss, {}, update));
    ASSERT_EQ(ErrorCodes::CollectionIsEmpty, storage.findSingleton(opCtx, nss));
    _assertDocumentsInCollectionEquals(opCtx, nss, std::vector<mongo::BSONObj>());
}

TEST_F(StorageInterfaceImplTest, UpdateSingletonUpdatesDocumentWhenCollectionIsNotEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    ASSERT_OK(
        storage.insertDocument(opCtx, nss, {doc1, Timestamp::min()}, OpTime::kUninitializedTerm));

    TimestampedBSONObj update;
    update.obj = BSON("$set" << BSON("x" << 1));
    update.timestamp = Timestamp();

    ASSERT_OK(storage.updateSingleton(opCtx, nss, BSON("_id" << 0), update));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 0 << "x" << 1),
                      unittest::assertGet(storage.findSingleton(opCtx, nss)));
    _assertDocumentsInCollectionEquals(opCtx, nss, {BSON("_id" << 0 << "x" << 1)});
}

TEST_F(StorageInterfaceImplTest, FindByIdThrowsIfUUIDNotInCatalog) {
    auto op = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto opCtx = getOperationContext();
    auto obj = BSON("_id"
                    << "kyle");
    StorageInterfaceImpl storage;
    ASSERT_THROWS_CODE(storage.findById(opCtx, {"local", UUID::gen()}, obj["_id"]).getStatus(),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(StorageInterfaceImplTest, FindByIdReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("nosuchdb.coll");
    auto doc = BSON("_id" << 0 << "x" << 0);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage.findById(opCtx, nss, doc["_id"]).getStatus());
}

TEST_F(StorageInterfaceImplTest, FindByIdReturnsNoSuchKeyWhenCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto doc = BSON("_id" << 0 << "x" << 0);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, storage.findById(opCtx, nss, doc["_id"]).getStatus());
}

TEST_F(StorageInterfaceImplTest, FindByIdReturnsNoSuchKeyWhenDocumentIsNotFound) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    auto doc2 = BSON("_id" << 1 << "x" << 1);
    auto doc3 = BSON("_id" << 2 << "x" << 2);
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {{doc1, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc3, Timestamp(0), OpTime::kUninitializedTerm}}));

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, storage.findById(opCtx, nss, doc2["_id"]).getStatus());
    ASSERT_EQUALS(
        ErrorCodes::NoSuchKey,
        storage.findById(opCtx, {nss.db().toString(), *options.uuid}, doc2["_id"]).getStatus());
}

TEST_F(StorageInterfaceImplTest, FindByIdReturnsDocumentWhenDocumentExists) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    auto doc2 = BSON("_id" << 1 << "x" << 1);
    auto doc3 = BSON("_id" << 2 << "x" << 2);
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {{doc1, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc2, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc3, Timestamp(0), OpTime::kUninitializedTerm}}));

    ASSERT_BSONOBJ_EQ(doc2, unittest::assertGet(storage.findById(opCtx, nss, doc2["_id"])));
    ASSERT_BSONOBJ_EQ(doc2,
                      unittest::assertGet(storage.findById(
                          opCtx, {nss.db().toString(), *options.uuid}, doc2["_id"])));
}

TEST_F(StorageInterfaceImplTest, FindByIdReturnsBadStatusIfPlanExecutorFails) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    auto doc2 = BSON("_id" << 1 << "x" << 1);
    auto doc3 = BSON("_id" << 2 << "x" << 2);
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {{doc1, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc2, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc3, Timestamp(0), OpTime::kUninitializedTerm}}));

    FailPointEnableBlock planExecKiller("planExecutorAlwaysFails");
    ASSERT_NOT_OK(storage.findById(opCtx, nss, doc2["_id"]));
}

TEST_F(StorageInterfaceImplTest, DeleteByIdThrowsIfUUIDNotInCatalog) {
    auto op = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto opCtx = getOperationContext();
    auto obj = BSON("_id"
                    << "kyle");
    StorageInterfaceImpl storage;
    ASSERT_THROWS_CODE(storage.deleteById(opCtx, {"local", UUID::gen()}, obj["_id"]).getStatus(),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(StorageInterfaceImplTest, DeleteByIdReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("nosuchdb.coll");
    auto doc = BSON("_id" << 0 << "x" << 0);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage.deleteById(opCtx, nss, doc["_id"]).getStatus());
}

TEST_F(StorageInterfaceImplTest, DeleteByIdReturnsNoSuchKeyWhenCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto doc = BSON("_id" << 0 << "x" << 0);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, storage.deleteById(opCtx, nss, doc["_id"]).getStatus());
    _assertDocumentsInCollectionEquals(opCtx, nss, std::vector<BSONObj>{});
}

TEST_F(StorageInterfaceImplTest, DeleteByIdReturnsNoSuchKeyWhenDocumentIsNotFound) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    auto doc2 = BSON("_id" << 1 << "x" << 1);
    auto doc3 = BSON("_id" << 2 << "x" << 2);
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {{doc1, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc3, Timestamp(0), OpTime::kUninitializedTerm}}));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, storage.deleteById(opCtx, nss, doc2["_id"]).getStatus());
    ASSERT_EQUALS(
        ErrorCodes::NoSuchKey,
        storage.deleteById(opCtx, {nss.db().toString(), *options.uuid}, doc2["_id"]).getStatus());
    _assertDocumentsInCollectionEquals(opCtx, nss, {doc1, doc3});
}

TEST_F(StorageInterfaceImplTest, DeleteByIdReturnsDocumentWhenDocumentExists) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));
    auto doc1 = BSON("_id" << 0 << "x" << 0);
    auto doc2 = BSON("_id" << 1 << "x" << 1);
    auto doc3 = BSON("_id" << 2 << "x" << 2);
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {{doc1, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc2, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc3, Timestamp(0), OpTime::kUninitializedTerm}}));

    ASSERT_BSONOBJ_EQ(doc2, unittest::assertGet(storage.deleteById(opCtx, nss, doc2["_id"])));
    _assertDocumentsInCollectionEquals(opCtx, nss, {doc1, doc3});

    ASSERT_BSONOBJ_EQ(doc1, unittest::assertGet(storage.deleteById(opCtx, nss, doc1["_id"])));
    _assertDocumentsInCollectionEquals(opCtx, nss, {doc3});
}

TEST_F(StorageInterfaceImplTest, UpsertByIdThrowsIfUUIDNotInCatalog) {
    auto op = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto opCtx = getOperationContext();
    auto obj = BSON("_id"
                    << "kyle");
    StorageInterfaceImpl storage;
    ASSERT_THROWS_CODE(storage.upsertById(opCtx, {"local", UUID::gen()}, obj["_id"], obj),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(StorageInterfaceImplTest,
       UpsertSingleDocumentReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("nosuchdb.coll");
    auto doc = BSON("_id" << 0 << "x" << 1);
    auto status = storage.upsertById(opCtx, nss, doc["_id"], doc);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
}

TEST_F(StorageInterfaceImplTest,
       UpsertSingleDocumentReturnsNamespaceNotFoundWhenCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("mydb.coll");
    NamespaceString wrongColl(nss.db(), "wrongColl"_sd);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto doc = BSON("_id" << 0 << "x" << 1);
    auto status = storage.upsertById(opCtx, wrongColl, doc["_id"], doc);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
    ASSERT_EQUALS("Collection [mydb.wrongColl] not found. Unable to update document.",
                  status.reason());
}

TEST_F(StorageInterfaceImplTest, UpsertSingleDocumentReplacesExistingDocumentInCollection) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));

    auto originalDoc = BSON("_id" << 1 << "x" << 1);
    ASSERT_OK(storage.insertDocuments(
        opCtx,
        nss,
        {{BSON("_id" << 0 << "x" << 0), Timestamp(0), OpTime::kUninitializedTerm},
         {originalDoc, Timestamp(0), OpTime::kUninitializedTerm},
         {BSON("_id" << 2 << "x" << 2), Timestamp(2), OpTime::kUninitializedTerm}}));

    ASSERT_OK(storage.upsertById(opCtx, nss, originalDoc["_id"], BSON("x" << 100)));
    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0 << "x" << 0),
                                        BSON("_id" << 1 << "x" << 100),
                                        BSON("_id" << 2 << "x" << 2)});

    // Again, but specify the collection's UUID.
    ASSERT_OK(storage.upsertById(
        opCtx, {nss.db().toString(), *options.uuid}, originalDoc["_id"], BSON("x" << 200)));
    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0 << "x" << 0),
                                        BSON("_id" << 1 << "x" << 200),
                                        BSON("_id" << 2 << "x" << 2)});
}

TEST_F(StorageInterfaceImplTest, UpsertSingleDocumentInsertsNewDocumentInCollectionIfIdIsNotFound) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));

    ASSERT_OK(storage.insertDocuments(
        opCtx,
        nss,
        {{BSON("_id" << 0 << "x" << 0), Timestamp(0), OpTime::kUninitializedTerm},
         {BSON("_id" << 2 << "x" << 2), Timestamp(2), OpTime::kUninitializedTerm}}));

    ASSERT_OK(storage.upsertById(opCtx, nss, BSON("" << 1).firstElement(), BSON("x" << 100)));

    // _assertDocumentsInCollectionEquals() reads collection in $natural order. Assumes new document
    // is inserted at end of collection.
    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0 << "x" << 0),
                                        BSON("_id" << 2 << "x" << 2),
                                        BSON("_id" << 1 << "x" << 100)});

    ASSERT_OK(storage.upsertById(opCtx,
                                 {nss.db().toString(), *options.uuid},
                                 BSON("" << 3).firstElement(),
                                 BSON("x" << 300)));
    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0 << "x" << 0),
                                        BSON("_id" << 2 << "x" << 2),
                                        BSON("_id" << 1 << "x" << 100),
                                        BSON("_id" << 3 << "x" << 300)});
}

TEST_F(StorageInterfaceImplTest,
       UpsertSingleDocumentReplacesExistingDocumentInIllegalClientSystemNamespace) {
    // Checks that we can update collections with namespaces not considered "legal client system"
    // namespaces.
    NamespaceString nss("local.system.rollback.docs");
    ASSERT_FALSE(nss.isLegalClientSystemNS(serverGlobalParams.featureCompatibility));

    auto opCtx = getOperationContext();
    auto options = generateOptionsWithUuid();
    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));

    auto originalDoc = BSON("_id" << 1 << "x" << 1);
    ASSERT_OK(storage.insertDocuments(
        opCtx,
        nss,
        {{BSON("_id" << 0 << "x" << 0), Timestamp(0), OpTime::kUninitializedTerm},
         {originalDoc, Timestamp(0), OpTime::kUninitializedTerm},
         {BSON("_id" << 2 << "x" << 2), Timestamp(2), OpTime::kUninitializedTerm}}));

    ASSERT_OK(storage.upsertById(opCtx, nss, originalDoc["_id"], BSON("x" << 100)));
    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0 << "x" << 0),
                                        BSON("_id" << 1 << "x" << 100),
                                        BSON("_id" << 2 << "x" << 2)});

    ASSERT_OK(storage.upsertById(
        opCtx, {nss.db().toString(), *options.uuid}, originalDoc["_id"], BSON("x" << 200)));
    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0 << "x" << 0),
                                        BSON("_id" << 1 << "x" << 200),
                                        BSON("_id" << 2 << "x" << 2)});
}

TEST_F(StorageInterfaceImplTest, UpsertSingleDocumentReturnsFailedToParseOnNonSimpleIdQuery) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));

    auto status = storage.upsertById(
        opCtx, nss, BSON("" << BSON("$gt" << 3)).firstElement(), BSON("x" << 100));
    ASSERT_EQUALS(ErrorCodes::InvalidIdField, status);
    ASSERT_STRING_CONTAINS(status.reason(),
                           "Unable to update document with a non-simple _id query:");

    ASSERT_EQ(storage.upsertById(opCtx,
                                 {nss.db().toString(), *options.uuid},
                                 BSON("" << BSON("$gt" << 3)).firstElement(),
                                 BSON("x" << 100)),
              ErrorCodes::InvalidIdField);
}

TEST_F(StorageInterfaceImplTest,
       UpsertSingleDocumentReturnsIndexNotFoundIfCollectionDoesNotHaveAnIdIndex) {
    CollectionOptions options = generateOptionsWithUuid();
    options.setNoIdIndex();

    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, options));

    auto doc = BSON("_id" << 0 << "x" << 100);
    auto status = storage.upsertById(opCtx, nss, doc["_id"], doc);
    ASSERT_EQUALS(ErrorCodes::IndexNotFound, status);
    ASSERT_STRING_CONTAINS(status.reason(),
                           "Unable to update document in a collection without an _id index.");

    ASSERT_EQ(storage.upsertById(opCtx, {nss.db().toString(), *options.uuid}, doc["_id"], doc),
              ErrorCodes::IndexNotFound);
}

TEST_F(StorageInterfaceImplTest,
       UpsertSingleDocumentReturnsFailedToParseWhenUpdateDocumentContainsUnknownOperator) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();

    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, options));

    auto unknownUpdateOp = BSON("$unknownUpdateOp" << BSON("x" << 1000));
    ASSERT_THROWS_CODE_AND_WHAT(
        storage.upsertById(opCtx, nss, BSON("" << 1).firstElement(), unknownUpdateOp),
        AssertionException,
        ErrorCodes::FailedToParse,
        "Unknown modifier: $unknownUpdateOp. Expected a valid update modifier or pipeline-style "
        "update specified as an array");

    ASSERT_THROWS_CODE(storage.upsertById(opCtx,
                                          {nss.db().toString(), *options.uuid},
                                          BSON("" << 1).firstElement(),
                                          unknownUpdateOp),
                       DBException,
                       ErrorCodes::FailedToParse);
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("nosuchdb.coll");
    auto filter = BSON("x" << 1);
    auto status = storage.deleteByFilter(opCtx, nss, filter);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterReturnsBadValueWhenFilterContainsUnknownOperator) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    auto filter = BSON("x" << BSON("$unknownFilterOp" << 1));
    auto status = storage.deleteByFilter(opCtx, nss, filter);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), "unknown operator: $unknownFilterOp");
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterOnCappedCollection) {
    transport::TransportLayerMock transportLayerMock;
    auto userClient = getOperationContext()->getServiceContext()->makeClient(
        "user", transportLayerMock.createSession());
    AlternativeClientRegion acr(userClient);
    const auto opCtx = cc().makeOperationContext();
    ASSERT(cc().isFromUserConnection());

    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    CollectionOptions options = generateOptionsWithUuid();
    options.capped = true;
    options.cappedSize = 1024 * 1024;
    ASSERT_OK(storage.createCollection(opCtx.get(), nss, options));

    auto filter = BSON("x" << 1);
    ASSERT_OK(storage.deleteByFilter(opCtx.get(), nss, filter));
}

TEST_F(
    StorageInterfaceImplTest,
    DeleteByFilterReturnsPrimarySteppedDownWhenCurrentMemberStateIsRollbackAndReplicatedWritesAreEnabled) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("mydb.mycoll");
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    auto doc = BSON("_id" << 0 << "x" << 0);
    ASSERT_OK(
        storage.insertDocuments(opCtx, nss, {{doc, Timestamp(0), OpTime::kUninitializedTerm}}));
    _assertDocumentsInCollectionEquals(opCtx, nss, {doc});

    // This test fixture disables replicated writes by default. We want to re-enable this setting
    // for this test.
    resetUnreplicatedWritesBlock();
    ASSERT_TRUE(opCtx->writesAreReplicated());

    // deleteByFilter() checks the current member state indirectly through
    // ReplicationCoordinator::canAcceptWrites() if replicated writes are enabled.
    ASSERT_OK(getReplicationCoordinatorMock()->setFollowerMode(MemberState::RS_ROLLBACK));

    auto filter = BSON("x" << 0);
    ASSERT_EQUALS(ErrorCodes::PrimarySteppedDown, storage.deleteByFilter(opCtx, nss, filter));
}

TEST_F(
    StorageInterfaceImplTest,
    DeleteByFilterReturnsPrimarySteppedDownWhenReplicationCoordinatorCannotAcceptWritesAndReplicatedWritesAreEnabled) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("mydb.mycoll");
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    auto doc = BSON("_id" << 0 << "x" << 0);
    ASSERT_OK(
        storage.insertDocuments(opCtx, nss, {{doc, Timestamp(0), OpTime::kUninitializedTerm}}));
    _assertDocumentsInCollectionEquals(opCtx, nss, {doc});

    // This test fixture disables replicated writes by default. We want to re-enable this setting
    // for this test.
    resetUnreplicatedWritesBlock();
    ASSERT_TRUE(opCtx->writesAreReplicated());

    // deleteByFilter() checks ReplicationCoordinator::canAcceptWritesFor() if replicated writes are
    // enabled on the OperationContext.
    getReplicationCoordinatorMock()->alwaysAllowWrites(false);

    auto filter = BSON("x" << 0);
    ASSERT_EQUALS(ErrorCodes::PrimarySteppedDown, storage.deleteByFilter(opCtx, nss, filter));
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterReturnsNamespaceNotFoundWhenCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("mydb.coll");
    NamespaceString wrongColl(nss.db(), "wrongColl"_sd);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto filter = BSON("x" << 1);
    auto status = storage.deleteByFilter(opCtx, wrongColl, filter);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
    ASSERT_EQUALS(std::string(
                      str::stream()
                      << "Collection [mydb.wrongColl] not found. Unable to delete documents in "
                      << wrongColl.ns() << " using filter " << filter),
                  status.reason());
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterReturnsSuccessIfCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    ASSERT_OK(storage.deleteByFilter(opCtx, nss, {}));

    _assertDocumentsInCollectionEquals(opCtx, nss, std::vector<BSONObj>{});
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterLeavesCollectionUnchangedIfNoDocumentsMatchFilter) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    std::vector<TimestampedBSONObj> docs = {{BSON("_id" << 0 << "x" << 0), Timestamp(0)},
                                            {BSON("_id" << 2 << "x" << 2), Timestamp(0)}};
    ASSERT_OK(storage.insertDocuments(opCtx, nss, transformInserts(docs)));

    auto filter = BSON("x" << 1);
    ASSERT_OK(storage.deleteByFilter(opCtx, nss, filter));

    _assertDocumentsInCollectionEquals(opCtx, nss, docs);
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterRemoveDocumentsThatMatchFilter) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    std::vector<TimestampedBSONObj> docs = {{BSON("_id" << 0 << "x" << 0), Timestamp(0)},
                                            {BSON("_id" << 1 << "x" << 1), Timestamp(0)},
                                            {BSON("_id" << 2 << "x" << 2), Timestamp(0)},
                                            {BSON("_id" << 3 << "x" << 3), Timestamp(0)}};
    ASSERT_OK(storage.insertDocuments(opCtx, nss, transformInserts(docs)));

    auto filter = BSON("x" << BSON("$in" << BSON_ARRAY(1 << 2)));
    ASSERT_OK(storage.deleteByFilter(opCtx, nss, filter));

    auto docsRemaining = {BSON("_id" << 0 << "x" << 0), BSON("_id" << 3 << "x" << 3)};
    _assertDocumentsInCollectionEquals(opCtx, nss, docsRemaining);
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterExpandsDottedFieldNamesAsPaths) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    std::vector<TimestampedBSONObj> docs = {
        {BSON("_id" << 0 << "x" << BSON("y" << 0)), Timestamp::min()},
        {BSON("_id" << 1 << "x" << BSON("y" << 1)), Timestamp::min()},
        {BSON("_id" << 2 << "x" << BSON("y" << 2)), Timestamp::min()},
        {BSON("_id" << 3 << "x" << BSON("y" << 3)), Timestamp::min()}};
    ASSERT_OK(storage.insertDocuments(opCtx, nss, transformInserts(docs)));

    auto filter = BSON("x.y" << BSON("$gte" << 1));
    ASSERT_OK(storage.deleteByFilter(opCtx, nss, filter));

    auto docsRemaining = {BSON("_id" << 0 << "x" << BSON("y" << 0))};
    _assertDocumentsInCollectionEquals(opCtx, nss, docsRemaining);
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterUsesIdHackIfFilterContainsIdFieldOnly) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    std::vector<TimestampedBSONObj> docs = {{BSON("_id" << 0 << "x" << 0), Timestamp(0)},
                                            {BSON("_id" << 1 << "x" << 1), Timestamp(0)}};
    ASSERT_OK(storage.insertDocuments(opCtx, nss, transformInserts(docs)));

    auto filter = BSON("_id" << 1);
    ASSERT_OK(storage.deleteByFilter(opCtx, nss, filter));

    auto docsRemaining = {BSON("_id" << 0 << "x" << 0)};
    _assertDocumentsInCollectionEquals(opCtx, nss, docsRemaining);
}

TEST_F(StorageInterfaceImplTest, DeleteByFilterRemovesDocumentsInIllegalClientSystemNamespace) {
    // Checks that we can remove documents from collections with namespaces not considered "legal
    // client system" namespaces.
    NamespaceString nss("local.system.rollback.docs");
    ASSERT_FALSE(nss.isLegalClientSystemNS(serverGlobalParams.featureCompatibility));

    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));

    std::vector<TimestampedBSONObj> docs = {{BSON("_id" << 0 << "x" << 0), Timestamp(0)},
                                            {BSON("_id" << 1 << "x" << 1), Timestamp(0)},
                                            {BSON("_id" << 2 << "x" << 2), Timestamp(0)},
                                            {BSON("_id" << 3 << "x" << 3), Timestamp(0)}};
    ASSERT_OK(storage.insertDocuments(opCtx, nss, transformInserts(docs)));

    auto filter = BSON("$or" << BSON_ARRAY(BSON("x" << 0) << BSON("_id" << 2)));
    ASSERT_OK(storage.deleteByFilter(opCtx, nss, filter));

    auto docsRemaining = {BSON("_id" << 1 << "x" << 1), BSON("_id" << 3 << "x" << 3)};
    _assertDocumentsInCollectionEquals(opCtx, nss, docsRemaining);
}

TEST_F(StorageInterfaceImplTest,
       DeleteByFilterRespectsCollectionsDefaultCollationWhenRemovingDocuments) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);

    // Create a collection using a case-insensitive collation.
    CollectionOptions options = generateOptionsWithUuid();
    options.collation = BSON("locale"
                             << "en_US"
                             << "strength" << 2);
    ASSERT_OK(storage.createCollection(opCtx, nss, options));

    auto doc1 = BSON("_id" << 1 << "x"
                           << "ABC");
    auto doc2 = BSON("_id" << 2 << "x"
                           << "abc");
    auto doc3 = BSON("_id" << 3 << "x"
                           << "DEF");
    auto doc4 = BSON("_id" << 4 << "x"
                           << "def");
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {{doc1, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc2, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc3, Timestamp(0), OpTime::kUninitializedTerm},
                                       {doc4, Timestamp(0), OpTime::kUninitializedTerm}}));

    // This filter should remove doc1 and doc2 because the values of the field "x"
    // are equivalent to "aBc" under the case-insensive collation.
    auto filter = BSON("x"
                       << "aBc");
    ASSERT_OK(storage.deleteByFilter(opCtx, nss, filter));

    _assertDocumentsInCollectionEquals(opCtx, nss, {doc3, doc4});
}

TEST_F(StorageInterfaceImplTest,
       GetCollectionCountReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("nosuchdb.coll");
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage.getCollectionCount(opCtx, nss).getStatus());
}

TEST_F(StorageInterfaceImplTest,
       GetCollectionCountReturnsNamespaceNotFoundWhenCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    NamespaceString wrongColl(nss.db(), "wrongColl"_sd);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage.getCollectionCount(opCtx, wrongColl).getStatus());
}

TEST_F(StorageInterfaceImplTest, GetCollectionCountReturnsZeroOnEmptyCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto count = unittest::assertGet(storage.getCollectionCount(opCtx, nss));
    ASSERT_EQUALS(0UL, count);
}

TEST_F(StorageInterfaceImplTest, GetCollectionCountReturnsCollectionCount) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    auto count = unittest::assertGet(storage.getCollectionCount(opCtx, nss));
    ASSERT_EQUALS(3UL, count);
}

TEST_F(StorageInterfaceImplTest,
       SetCollectionCountReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("nosuchdb.coll");
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, storage.setCollectionCount(opCtx, nss, 3));
}

TEST_F(StorageInterfaceImplTest,
       SetCollectionCountReturnsNamespaceNotFoundWhenCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    NamespaceString wrongColl(nss.db(), "wrongColl"_sd);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, storage.setCollectionCount(opCtx, wrongColl, 3));
}

TEST_F(StorageInterfaceImplTest,
       GetCollectionSizeReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, storage.getCollectionSize(opCtx, nss).getStatus());
}

TEST_F(StorageInterfaceImplTest,
       GetCollectionSizeReturnsNamespaceNotFoundWhenCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    NamespaceString wrongColl(nss.db(), "wrongColl"_sd);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage.getCollectionSize(opCtx, wrongColl).getStatus());
}

TEST_F(StorageInterfaceImplTest, GetCollectionSizeReturnsZeroOnEmptyCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    auto size = unittest::assertGet(storage.getCollectionSize(opCtx, nss));
    ASSERT_EQUALS(0UL, size);
}

TEST_F(StorageInterfaceImplTest, GetCollectionSizeReturnsCollectionSize) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, generateOptionsWithUuid()));
    ASSERT_OK(
        storage.insertDocuments(opCtx,
                                nss,
                                {{BSON("_id" << 1), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 2), Timestamp(0), OpTime::kUninitializedTerm},
                                 {BSON("_id" << 0), Timestamp(0), OpTime::kUninitializedTerm}}));
    auto size = unittest::assertGet(storage.getCollectionSize(opCtx, nss));
    ASSERT_NOT_EQUALS(0UL, size);
}

TEST_F(StorageInterfaceImplTest, SetIndexIsMultikeyReturnsNamespaceNotFoundForMissingDatabase) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_EQUALS(
        ErrorCodes::NamespaceNotFound,
        storage.setIndexIsMultikey(opCtx, nss, UUID::gen(), "foo", {}, {}, Timestamp(3, 3)));
}

TEST_F(StorageInterfaceImplTest, SetIndexIsMultikeyReturnsNamespaceNotFoundForMissingCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    NamespaceString wrongColl(nss.db(), "wrongColl"_sd);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_EQUALS(
        ErrorCodes::NamespaceNotFound,
        storage.setIndexIsMultikey(opCtx, wrongColl, UUID::gen(), "foo", {}, {}, Timestamp(3, 3)));
}

TEST_F(StorageInterfaceImplTest, SetIndexIsMultikeyLooksUpCollectionByUUID) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();
    ASSERT_OK(storage.createCollection(opCtx, nss, options));
    NamespaceString wrongColl(nss.db(), "wrongColl"_sd);
    ASSERT_EQUALS(ErrorCodes::IndexNotFound,
                  storage.setIndexIsMultikey(
                      opCtx, wrongColl, *options.uuid, "foo", {}, {}, Timestamp(3, 3)));
}

TEST_F(StorageInterfaceImplTest, SetIndexIsMultikeyReturnsIndexNotFoundForMissingIndex) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();
    ASSERT_OK(storage.createCollection(opCtx, nss, options));
    ASSERT_EQUALS(
        ErrorCodes::IndexNotFound,
        storage.setIndexIsMultikey(opCtx, nss, *options.uuid, "foo", {}, {}, Timestamp(3, 3)));
}

TEST_F(StorageInterfaceImplTest, SetIndexIsMultikeyReturnsInvalidOptionsForNullTimestamp) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();
    ASSERT_OK(storage.createCollection(opCtx, nss, options));
    ASSERT_EQUALS(
        ErrorCodes::InvalidOptions,
        storage.setIndexIsMultikey(opCtx, nss, *options.uuid, "foo", {}, {}, Timestamp()));
}

TEST_F(StorageInterfaceImplTest, SetIndexIsMultikeySucceeds) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto options = generateOptionsWithUuid();
    ASSERT_OK(storage.createCollection(opCtx, nss, options));

    auto indexName = "a_b_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a.b" << 1) << "v"
                                 << static_cast<int>(kIndexVersion));
    ASSERT_EQUALS(_createIndexOnEmptyCollection(opCtx, nss, indexSpec), 2);

    MultikeyPaths paths = {{1}};
    ASSERT_OK(storage.setIndexIsMultikey(
        opCtx, nss, *options.uuid, indexName, {}, paths, Timestamp(3, 3)));
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_TRUE(autoColl.getCollection());
    auto indexCatalog = autoColl.getCollection()->getIndexCatalog();
    auto entry = indexCatalog->findIndexByName(opCtx, indexName)->getEntry();
    ASSERT(entry->isMultikey(opCtx, autoColl.getCollection()));
    ASSERT(paths == entry->getMultikeyPaths(opCtx, autoColl.getCollection()));
}

}  // namespace

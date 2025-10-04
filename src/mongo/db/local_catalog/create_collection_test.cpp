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

#include "mongo/db/local_catalog/create_collection.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/virtual_collection_impl.h"
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/stdx/utility.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
using namespace std::string_literals;

class CreateCollectionTest : public ServiceContextMongoDTest {
protected:
    void setUp() override;
    void tearDown() override;

    void validateValidator(const std::string& validatorStr, int expectedError);

    // Use StorageInterface to access storage features below catalog interface.
    std::unique_ptr<repl::StorageInterface> _storage;
};

void CreateCollectionTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();

    // Set up ReplicationCoordinator and ensure that we are primary.
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::ReplicationCoordinator::set(service, std::move(replCoord));

    _storage = std::make_unique<repl::StorageInterfaceImpl>();
}

void CreateCollectionTest::tearDown() {
    _storage = {};

    // Tear down mongod.
    ServiceContextMongoDTest::tearDown();
}

class CreateVirtualCollectionTest : public CreateCollectionTest {
private:
    void setUp() override {
        CreateCollectionTest::setUp();
        computeModeEnabled = true;
    }
    void tearDown() override {
        computeModeEnabled = false;
        CreateCollectionTest::tearDown();
    }
};

/**
 * Creates an OperationContext.
 */
ServiceContext::UniqueOperationContext makeOpCtx() {
    auto opCtx = cc().makeOperationContext();
    repl::createOplog(opCtx.get());
    return opCtx;
}

void CreateCollectionTest::validateValidator(const std::string& validatorStr,
                                             const int expectedError) {
    NamespaceString newNss =
        NamespaceString::createNamespaceString_forTest("test.newCollWithValidation");

    auto opCtx = makeOpCtx();

    CollectionOptions options;
    options.validator = fromjson(validatorStr);
    options.uuid = UUID::gen();

    return writeConflictRetry(opCtx.get(), "create", newNss, [&] {
        AutoGetCollection autoColl(opCtx.get(), newNss, MODE_IX);
        auto db = autoColl.ensureDbExists(opCtx.get());
        ASSERT_TRUE(db) << "Cannot create collection " << newNss.toStringForErrorMsg()
                        << " because database " << newNss.dbName().toStringForErrorMsg()
                        << " does not exist.";

        WriteUnitOfWork wuow(opCtx.get());
        const auto status =
            db->userCreateNS(opCtx.get(), newNss, options, false /*createDefaultIndexes*/);
        ASSERT_EQ(expectedError, status.code()) << status;
    });
}

CollectionAcquisition acquireCollForRead(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}

/**
 * Returns true if collection exists.
 */
bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollForRead(opCtx, nss).exists();
}

/**
 * Returns collection options.
 */
CollectionOptions getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
    const auto collection = acquireCollForRead(opCtx, nss);
    ASSERT_TRUE(collection.exists())
        << "Unable to get collections options for " << nss.toStringForErrorMsg()
        << " because collection does not exist.";
    return collection.getCollectionPtr()->getCollectionOptions();
}

/**
 * Returns a VirtualCollectionImpl if the underlying implementation object is a virtual collection.
 */
const VirtualCollectionImpl* getVirtualCollection(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    const auto collection = acquireCollForRead(opCtx, nss);
    return dynamic_cast<const VirtualCollectionImpl*>(collection.getCollectionPtr().get());
}

/**
 * Returns virtual collection options.
 */
VirtualCollectionOptions getVirtualCollectionOptions(OperationContext* opCtx,
                                                     const NamespaceString& nss) {
    auto vcollPtr = getVirtualCollection(opCtx, nss);
    ASSERT_TRUE(vcollPtr) << "Collection must be a virtual collection";

    return vcollPtr->getVirtualCollectionOptions();
}

/**
 * Returns UUID of collection.
 */
UUID getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    auto options = getCollectionOptions(opCtx, nss);
    ASSERT_TRUE(options.uuid);
    return *(options.uuid);
}

TEST_F(CreateCollectionTest, CreateCollectionForApplyOpsWithSpecificUuidNoExistingCollection) {
    NamespaceString newNss = NamespaceString::createNamespaceString_forTest("test.newColl");

    auto opCtx = makeOpCtx();
    ASSERT_FALSE(collectionExists(opCtx.get(), newNss));

    auto uuid = UUID::gen();
    Lock::DBLock lock(opCtx.get(), newNss.dbName(), MODE_IX);
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          newNss.dbName(),
                                          uuid,
                                          BSON("create" << newNss.coll()),
                                          /*allowRenameOutOfTheWay*/ false));

    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
}

TEST_F(CreateCollectionTest, CreateCollectionForApplyOpsWithSpecificUuidNestedInObject) {
    // Documents the behavior where 'uuid' is retrieved from the 'o' field when not provided  in
    // 'ui'. This differs from create oplog entries generated by a primary node - where 'uuid' is
    // omitted from the 'o' field and instead stored in the 'ui' field of the oplog entry.
    //
    // The test ensures that future code changes do not break any downstream tools that rely on the
    // pre-existing behavior.
    NamespaceString newNss = NamespaceString::createNamespaceString_forTest("test.newColl");

    auto opCtx = makeOpCtx();
    ASSERT_FALSE(collectionExists(opCtx.get(), newNss));

    auto uuid = UUID::gen();
    Lock::DBLock lock(opCtx.get(), newNss.dbName(), MODE_IX);
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          newNss.dbName(),
                                          boost::none,
                                          BSON("create" << newNss.coll() << "uuid" << uuid),
                                          /*allowRenameOutOfTheWay*/ false));

    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
    ASSERT_EQUALS(uuid, getCollectionUuid(opCtx.get(), newNss));
}

TEST_F(CreateCollectionTest,
       CreateCollectionForApplyOpsWithSpecificUuidNonDropPendingCurrentCollectionHasSameUuid) {
    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    NamespaceString newNss = NamespaceString::createNamespaceString_forTest("test.newColl");

    auto opCtx = makeOpCtx();
    auto uuid = UUID::gen();
    Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.

    // Create existing collection using StorageInterface.
    {
        CollectionOptions options;
        options.uuid = uuid;
        ASSERT_OK(_storage->createCollection(opCtx.get(), curNss, options));
    }
    ASSERT_TRUE(collectionExists(opCtx.get(), curNss));
    ASSERT_FALSE(collectionExists(opCtx.get(), newNss));

    // This should rename the existing collection 'curNss' to the collection 'newNss' we are trying
    // to create.
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          newNss.dbName(),
                                          uuid,
                                          BSON("create" << newNss.coll()),
                                          /*allowRenameOutOfTheWay*/ true));

    ASSERT_FALSE(collectionExists(opCtx.get(), curNss));
    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
}

TEST_F(CreateCollectionTest,
       CreateCollectionForApplyOpsWithSpecificUuidRenamesExistingCollectionWithSameNameOutOfWay) {
    NamespaceString newNss = NamespaceString::createNamespaceString_forTest("test.newColl");

    auto opCtx = makeOpCtx();
    auto uuid = UUID::gen();
    Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.

    // Create existing collection with same name but different UUID using StorageInterface.
    auto existingCollectionUuid = UUID::gen();
    {
        CollectionOptions options;
        options.uuid = existingCollectionUuid;
        ASSERT_OK(_storage->createCollection(opCtx.get(), newNss, options));
    }
    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
    ASSERT_NOT_EQUALS(uuid, getCollectionUuid(opCtx.get(), newNss));

    // This should rename the existing collection 'newNss' to a randomly generated collection name.
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          newNss.dbName(),
                                          uuid,
                                          BSON("create" << newNss.coll()),
                                          /*allowRenameOutOfTheWay*/ true));

    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
    ASSERT_EQUALS(uuid, getCollectionUuid(opCtx.get(), newNss));

    // Check that old collection that was renamed out of the way still exists.
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto renamedCollectionNss = catalog->lookupNSSByUUID(opCtx.get(), existingCollectionUuid);
    ASSERT(renamedCollectionNss);
    ASSERT_TRUE(collectionExists(opCtx.get(), *renamedCollectionNss))
        << "old renamed collection with UUID " << existingCollectionUuid
        << " missing: " << (*renamedCollectionNss).toStringForErrorMsg();
}

/**
 * Testing an oplog sequence that can cause inconsistent data being read:
 * 1. [TS1: Create timeseries coll "test.curColl", together with a system bucket
 *          "test.system.buckets.curColl" with UUID2]
 * 2. [TS2: Drop timeseries coll "test.curColl"]
 * 3. [TS3: Create timeseries coll "test.curColl" together with a system bucket
 *          "test.system.buckets.curColl" with UUID1]
 *
 * After initial sync, TS1 is applied and renames UUID1 away due to name conflict.
 * Renaming needs to respect the system.buckets collection prefix when collections are listed.
 */
TEST_F(CreateCollectionTest, CreateCollectionForApplyOpsRespectsTimeseriesBucketsCollectionPrefix) {
    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");
    auto opCtx = makeOpCtx();
    Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.

    // Create a time series collection
    const auto tsOptions = TimeseriesOptions("t");
    CreateCommand cmd = CreateCommand(curNss);
    cmd.getCreateCollectionRequest().setTimeseries(tsOptions);
    uassertStatusOK(createCollection(opCtx.get(), cmd));
    ASSERT_TRUE(collectionExists(opCtx.get(), bucketsColl));

    // The system.buckets collection was created with uuid1.
    auto uuid1 = getCollectionUuid(opCtx.get(), bucketsColl);
    // Now call createCollectionForApplyOps with the same name but a different uuid2.
    auto uuid2 = UUID::gen();
    ASSERT_NOT_EQUALS(uuid1, uuid2);
    // This should rename the old collection to a randomly generated collection name.
    cmd = CreateCommand(bucketsColl);
    cmd.getCreateCollectionRequest().setTimeseries(tsOptions);
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          bucketsColl.dbName(),
                                          uuid2,
                                          cmd.toBSON(),
                                          /*allowRenameOutOfTheWay*/ true));
    ASSERT_TRUE(collectionExists(opCtx.get(), bucketsColl));
    ASSERT_EQUALS(uuid2, getCollectionUuid(opCtx.get(), bucketsColl));

    // Check that old collection that was renamed out of the way still exists.
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto renamedCollectionNss = catalog->lookupNSSByUUID(opCtx.get(), uuid1);
    ASSERT(renamedCollectionNss);
    ASSERT_TRUE(collectionExists(opCtx.get(), *renamedCollectionNss))
        << "old renamed collection with UUID " << uuid1
        << " missing: " << (*renamedCollectionNss).toStringForErrorMsg();
    // The renamed collection should still have the system.buckets prefix.
    ASSERT(renamedCollectionNss->isTimeseriesBucketsCollection());

    // Drop the new collection (with uuid2).
    {
        repl::UnreplicatedWritesBlock uwb(opCtx.get());  // Do not use oplog.
        ASSERT_OK(_storage->dropCollection(opCtx.get(), bucketsColl));
    }
    ASSERT_FALSE(collectionExists(opCtx.get(), bucketsColl));

    // Now call createCollectionForApplyOps with uuid1.
    cmd = CreateCommand(bucketsColl);
    cmd.getCreateCollectionRequest().setTimeseries(tsOptions);
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          bucketsColl.dbName(),
                                          uuid1,
                                          cmd.toBSON(),
                                          /*allowRenameOutOfTheWay*/ false));
    // This should rename the old collection back to its original name.
    ASSERT_FALSE(collectionExists(opCtx.get(), *renamedCollectionNss));
    ASSERT_TRUE(collectionExists(opCtx.get(), bucketsColl));
    ASSERT_EQUALS(uuid1, getCollectionUuid(opCtx.get(), bucketsColl));
}

TEST_F(CreateCollectionTest, TimeseriesBucketingParametersChangedFlagNotSetIfFeatureDisabled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTSBucketingParametersUnchanged", false);
    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");

    auto opCtx = makeOpCtx();
    auto tsOptions = TimeseriesOptions("t");
    CreateCommand cmd = CreateCommand(curNss);
    cmd.getCreateCollectionRequest().setTimeseries(std::move(tsOptions));
    uassertStatusOK(createCollection(opCtx.get(), cmd));

    ASSERT_TRUE(collectionExists(opCtx.get(), bucketsColl));
    const auto collForRead = acquireCollForRead(opCtx.get(), bucketsColl);
    ASSERT_FALSE(collForRead.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
}

TEST_F(CreateCollectionTest,
       TimeseriesBucketingParametersChangedFlagNotSetIfCollectionNotTimeseries) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTSBucketingParametersUnchanged", false);
    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");

    auto opCtx = makeOpCtx();
    uassertStatusOK(createCollection(opCtx.get(), CreateCommand(curNss)));

    ASSERT_TRUE(collectionExists(opCtx.get(), curNss));
    const auto collForRead = acquireCollForRead(opCtx.get(), curNss);
    ASSERT_FALSE(collForRead.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
}

TEST_F(CreateCollectionTest, TimeseriesBucketingParametersChangedFlagTrue) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTSBucketingParametersUnchanged", true);

    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");
    auto bucketsColl =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.curColl");

    auto opCtx = makeOpCtx();
    auto tsOptions = TimeseriesOptions("t");
    CreateCommand cmd = CreateCommand(curNss);
    cmd.getCreateCollectionRequest().setTimeseries(std::move(tsOptions));
    uassertStatusOK(createCollection(opCtx.get(), cmd));

    ASSERT_TRUE(collectionExists(opCtx.get(), bucketsColl));
    const auto bucketsCollForRead = acquireCollForRead(opCtx.get(), bucketsColl);
    // TODO(SERVER-101611): Set *timeseriesBucketingParametersHaveChanged to false on create
    ASSERT_FALSE(bucketsCollForRead.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
}

TEST_F(CreateCollectionTest, TimeseriesBucketingParametersChangedFlagFalse) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTSBucketingParametersUnchanged", true);

    NamespaceString curNss = NamespaceString::createNamespaceString_forTest("test.curColl");

    auto opCtx = makeOpCtx();
    uassertStatusOK(createCollection(opCtx.get(), CreateCommand(curNss)));

    ASSERT_TRUE(collectionExists(opCtx.get(), curNss));
    const auto collForRead = acquireCollForRead(opCtx.get(), curNss);
    ASSERT_FALSE(collForRead.getCollectionPtr()->timeseriesBucketingParametersHaveChanged());
}


TEST_F(CreateCollectionTest, ValidationOptions) {
    // Try a valid validator before trying invalid validators.
    validateValidator("", static_cast<int>(ErrorCodes::Error::OK));
    validateValidator("{a: {$exists: false}}", static_cast<int>(ErrorCodes::Error::OK));

    // Invalid validators.
    validateValidator(
        "{$expr: {$function: {body: 'function(age) { return age >= 21; }', args: ['$age'], lang: "
        "'js'}}}",
        4660800);
    validateValidator("{$expr: {$_internalJsEmit: {eval: 'function() {}', this: {}}}}", 4660801);
    validateValidator("{$where: 'this.a == this.b'}", static_cast<int>(ErrorCodes::BadValue));
}

// Tests that validator validation is disabled when inserting a document into a
// <database>.system.resharding.* collection. The primary donor is responsible for validating
// documents before they are inserted into the recipient's temporary resharding collection.
TEST_F(CreateCollectionTest, ValidationDisabledForTemporaryReshardingCollection) {
    NamespaceString reshardingNss =
        NamespaceString::createNamespaceString_forTest("myDb", "system.resharding.yay");
    auto opCtx = makeOpCtx();

    {
        Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.
        BSONObj createCmdObj =
            BSON("create" << reshardingNss.coll() << "validator" << BSON("a" << 5));
        ASSERT_OK(createCollection(opCtx.get(), reshardingNss.dbName(), createCmdObj));
        ASSERT_TRUE(collectionExists(opCtx.get(), reshardingNss));
    }

    const auto collection = acquireCollection(
        opCtx.get(),
        CollectionAcquisitionRequest(reshardingNss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx.get()),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);

    WriteUnitOfWork wuow(opCtx.get());
    // Ensure a document that violates validator criteria can be inserted into the temporary
    // resharding collection.
    auto insertObj = fromjson("{'_id':2, a:1}");
    auto status = collection_internal::insertDocument(
        opCtx.get(), collection.getCollectionPtr(), InsertStatement(insertObj), nullptr, false);
    ASSERT_OK(status);
}

TEST_F(CreateCollectionTest, CreateCollectionForApplyOpsDoesNotCreateIdIndexOnClusteredCollection) {
    auto opCtx = makeOpCtx();
    auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    auto uuid = UUID::gen();

    {
        Lock::GlobalLock lk(opCtx.get(), MODE_X);
        ASSERT_OK(createCollectionForApplyOps(
            opCtx.get(),
            nss.dbName(),
            uuid,
            BSON("create" << nss.coll() << "clusteredIndex"
                          << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                      << "clustered_index"
                                      << "unique" << true)),
            /*allowRenameOutOfTheWay*/ false));
    }

    auto coll = acquireCollForRead(opCtx.get(), nss);
    auto indexCatalog = coll.getCollectionPtr()->getIndexCatalog();
    ASSERT_FALSE(indexCatalog->haveAnyIndexes());
}

TEST_F(CreateCollectionTest, CreateCollectionForApplyOpsUsesDefaultIdIndexSpecIfEmpty) {
    auto opCtx = makeOpCtx();
    auto noIdNss = NamespaceString::createNamespaceString_forTest("test.noId");
    auto emptySpecNss = NamespaceString::createNamespaceString_forTest("test.emptyIdSpec");


    {
        Lock::GlobalLock lk(opCtx.get(), MODE_X);
        ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                              noIdNss.dbName(),
                                              UUID::gen(),
                                              BSON("create" << noIdNss.coll()),
                                              false,
                                              boost::none));
        ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                              emptySpecNss.dbName(),
                                              UUID::gen(),
                                              BSON("create" << emptySpecNss.coll()),
                                              false,
                                              BSONObj()));
    }

    auto noIdIndex = acquireCollForRead(opCtx.get(), noIdNss);
    ASSERT_FALSE(noIdIndex.getCollectionPtr()->isIndexPresent("_id_"));
    auto emptySpec = acquireCollForRead(opCtx.get(), emptySpecNss);
    ASSERT(emptySpec.getCollectionPtr()->isIndexPresent("_id_"));
}

TEST_F(CreateCollectionTest, CreateCollectionForApplyOpsUsesIdIndexIdentIfSupplied) {
    auto opCtx = makeOpCtx();
    auto noIdNss = NamespaceString::createNamespaceString_forTest("test.noId");
    auto explicitIdNss = NamespaceString::createNamespaceString_forTest("test.explicitId");
    auto implicitIdNss = NamespaceString::createNamespaceString_forTest("test.implicitId");

    // Internal collections have id indexes and we need to filter out the idents for those later
    auto originalIdents = opCtx->getServiceContext()->getStorageEngine()->getEngine()->getAllIdents(
        *shard_role_details::getRecoveryUnit(opCtx.get()));

    {
        // Create three collections: one with no id index (but an ident specified), one with an id
        // index and an ident specified, and one with an id index but no ident specified
        Lock::GlobalLock lk(opCtx.get(), MODE_X);
        const auto idIndexSpec = BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                          << "_id_");
        ASSERT_OK(createCollectionForApplyOps(
            opCtx.get(),
            noIdNss.dbName(),
            UUID::gen(),
            BSON("create" << noIdNss.coll()),
            false,
            boost::none,
            CreateCollCatalogIdentifier{.ident = "collection-1", .idIndexIdent = "index-1"s}));
        ASSERT_OK(createCollectionForApplyOps(
            opCtx.get(),
            explicitIdNss.dbName(),
            UUID::gen(),
            BSON("create" << explicitIdNss.coll()),
            false,
            idIndexSpec,
            CreateCollCatalogIdentifier{.ident = "collection-2", .idIndexIdent = "index-2"s}));
        ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                              implicitIdNss.dbName(),
                                              UUID::gen(),
                                              BSON("create" << implicitIdNss.coll()),
                                              false,
                                              idIndexSpec,
                                              boost::none));
    }

    auto noIdIndex = acquireCollForRead(opCtx.get(), noIdNss);
    ASSERT_FALSE(noIdIndex.getCollectionPtr()->isIndexPresent("_id_"));

    auto explicitIdent = acquireCollForRead(opCtx.get(), explicitIdNss);
    ASSERT(explicitIdent.getCollectionPtr()->isIndexPresent("_id_"));
    ASSERT(explicitIdent.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(opCtx.get(),
                                                                                 "index-2"));
    auto implicitIdent = acquireCollForRead(opCtx.get(), implicitIdNss);
    ASSERT(explicitIdent.getCollectionPtr()->isIndexPresent("_id_"));

    // There should be exactly two new idents starting with index- once we exclude the idents which
    // existed before we created out collections. One should be "index-2" and one should be a newly
    // generated ident. "ident-1" should *not* be present as that collection doesn't have an id
    // index.
    auto newIdents = opCtx->getServiceContext()->getStorageEngine()->getEngine()->getAllIdents(
        *shard_role_details::getRecoveryUnit(opCtx.get()));
    std::set<StringData> indexIdents;
    for (auto& ident : newIdents) {
        if (ident.starts_with("index-"))
            indexIdents.insert(ident);
    }
    for (auto& ident : originalIdents) {
        indexIdents.erase(ident);
    }
    ASSERT_FALSE(indexIdents.contains("index-1"));
    ASSERT(indexIdents.contains("index-2"));
    ASSERT_EQ(indexIdents.size(), 2);
}

const auto kValidUrl1 = ExternalDataSourceMetadata::kUrlProtocolFile + "named_pipe1"s;
const auto kValidUrl2 = ExternalDataSourceMetadata::kUrlProtocolFile + "named_pipe2"s;

TEST_F(CreateVirtualCollectionTest, VirtualCollectionOptionsWithOneSource) {
    NamespaceString vcollNss = NamespaceString::createNamespaceString_forTest("myDb", "vcoll.name");
    auto opCtx = makeOpCtx();

    Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.
    VirtualCollectionOptions reqVcollOpts;
    reqVcollOpts.dataSources.emplace_back(kValidUrl1, StorageTypeEnum::pipe, FileTypeEnum::bson);
    ASSERT_OK(createVirtualCollection(opCtx.get(), vcollNss, reqVcollOpts));
    ASSERT_TRUE(getVirtualCollection(opCtx.get(), vcollNss));

    ASSERT_EQ(stdx::to_underlying(getCollectionOptions(opCtx.get(), vcollNss).autoIndexId),
              stdx::to_underlying(CollectionOptions::NO));

    auto vcollOpts = getVirtualCollectionOptions(opCtx.get(), vcollNss);
    ASSERT_EQ(vcollOpts.dataSources.size(), 1);
    ASSERT_EQ(vcollOpts.dataSources[0].url, kValidUrl1);
    ASSERT_EQ(stdx::to_underlying(vcollOpts.dataSources[0].storageType),
              stdx::to_underlying(StorageTypeEnum::pipe));
    ASSERT_EQ(stdx::to_underlying(vcollOpts.dataSources[0].fileType),
              stdx::to_underlying(FileTypeEnum::bson));
}

TEST_F(CreateVirtualCollectionTest, VirtualCollectionOptionsWithMultiSource) {
    NamespaceString vcollNss = NamespaceString::createNamespaceString_forTest("myDb", "vcoll.name");
    auto opCtx = makeOpCtx();

    Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.
    VirtualCollectionOptions reqVcollOpts;
    reqVcollOpts.dataSources.emplace_back(kValidUrl1, StorageTypeEnum::pipe, FileTypeEnum::bson);
    reqVcollOpts.dataSources.emplace_back(kValidUrl2, StorageTypeEnum::pipe, FileTypeEnum::bson);

    ASSERT_OK(createVirtualCollection(opCtx.get(), vcollNss, reqVcollOpts));
    ASSERT_TRUE(getVirtualCollection(opCtx.get(), vcollNss));

    ASSERT_EQ(stdx::to_underlying(getCollectionOptions(opCtx.get(), vcollNss).autoIndexId),
              stdx::to_underlying(CollectionOptions::NO));

    auto vcollOpts = getVirtualCollectionOptions(opCtx.get(), vcollNss);
    ASSERT_EQ(vcollOpts.dataSources.size(), 2);
    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(vcollOpts.dataSources[i].url, reqVcollOpts.dataSources[i].url);
        ASSERT_EQ(stdx::to_underlying(vcollOpts.dataSources[i].storageType),
                  stdx::to_underlying(StorageTypeEnum::pipe));
        ASSERT_EQ(stdx::to_underlying(vcollOpts.dataSources[i].fileType),
                  stdx::to_underlying(FileTypeEnum::bson));
    }
}

TEST_F(CreateVirtualCollectionTest, InvalidVirtualCollectionOptions) {
    NamespaceString vcollNss = NamespaceString::createNamespaceString_forTest("myDb", "vcoll.name");
    auto opCtx = makeOpCtx();

    Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.

    {
        bool exceptionOccurred = false;
        VirtualCollectionOptions reqVcollOpts;
        constexpr auto kInvalidUrl = "fff://abc/named_pipe"_sd;
        try {
            reqVcollOpts.dataSources.emplace_back(
                kInvalidUrl, StorageTypeEnum::pipe, FileTypeEnum::bson);
        } catch (const DBException&) {
            exceptionOccurred = true;
        }

        ASSERT_TRUE(exceptionOccurred)
            << fmt::format("Invalid 'url': {} must fail but succeeded", kInvalidUrl);
    }

    {
        bool exceptionOccurred = false;
        VirtualCollectionOptions reqVcollOpts;
        constexpr auto kInvalidStorageTypeEnum = StorageTypeEnum(2);
        try {
            reqVcollOpts.dataSources.emplace_back(
                kValidUrl1, kInvalidStorageTypeEnum, FileTypeEnum::bson);
        } catch (const DBException&) {
            exceptionOccurred = true;
        }

        ASSERT_TRUE(exceptionOccurred)
            << fmt::format("Unknown 'storageType': {} must fail but succeeded",
                           stdx::to_underlying(kInvalidStorageTypeEnum));
    }

    {
        bool exceptionOccurred = false;
        VirtualCollectionOptions reqVcollOpts;
        constexpr auto kInvalidFileTypeEnum = FileTypeEnum(2);
        try {
            reqVcollOpts.dataSources.emplace_back(
                kValidUrl1, StorageTypeEnum::pipe, kInvalidFileTypeEnum);
        } catch (const DBException&) {
            exceptionOccurred = true;
        }

        ASSERT_TRUE(exceptionOccurred)
            << fmt::format("Unknown 'fileType': {} must fail but succeeded",
                           stdx::to_underlying(kInvalidFileTypeEnum));
    }
}
}  // namespace
}  // namespace mongo

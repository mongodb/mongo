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

#include "mongo/db/local_catalog/database.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/index_builds/index_build_block.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database_holder_impl.h"
#include "mongo/db/local_catalog/database_impl.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/unique_collection_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_mock.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/pcre.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

class DatabaseTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    explicit DatabaseTest(Options options = {}) : ServiceContextMongoDTest(std::move(options)) {}

    ServiceContext::UniqueOperationContext _opCtx;
    NamespaceString _nss;
    OpObserverRegistry* _opObserverRegistry;
};

void DatabaseTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    _opCtx = cc().makeOperationContext();

    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());

    // Set up ReplicationCoordinator and create oplog.
    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));
    repl::createOplog(_opCtx.get());

    // Ensure that we are primary.
    auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Set up OpObserver so that Database will append actual oplog entries to the oplog using
    // repl::logOp(). repl::logOp() will also store the oplog entry's optime in ReplClientInfo.
    _opObserverRegistry = dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
    _opObserverRegistry->addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerMock>()));

    _nss = NamespaceString::createNamespaceString_forTest("test.foo");
}

void DatabaseTest::tearDown() {
    _nss = {};
    _opCtx = {};

    auto service = getServiceContext();
    repl::StorageInterface::set(service, {});

    ServiceContextMongoDTest::tearDown();
}

boost::optional<durable_catalog::CatalogEntry> getLocalCatalogEntry(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    const auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    const auto mdbCatalog = storageEngine->getMDBCatalog();
    return durable_catalog::scanForCatalogEntryByNss(opCtx, nss, mdbCatalog);
}

// OpObserver to validate the presence or absence of 'CreateCollCatalogIdentifier' after a create
// collection operation.
class CreateCollCatalogIdentifierObserver : public OpObserverNoop {
public:
    // - 'expectIdentifiers': If true, enforces calls to 'onCreateCollection()' report a
    //          'CreateCollCatalogIdentifier'. Otherwise, enforce 'CreateCollCatalogIdentifier' is
    //          always 'boost::none'. Provides an extra guarantee that each test targets the
    //          expected behavior.
    explicit CreateCollCatalogIdentifierObserver(bool expectIdentifiers)
        : _expectIdentifiers(expectIdentifiers) {}

    void onCreateCollection(
        OperationContext* opCtx,
        const NamespaceString& collectionName,
        const CollectionOptions& options,
        const BSONObj& idIndex,
        const OplogSlot& createOpTime,
        const boost::optional<CreateCollCatalogIdentifier>& collCatalogIdentifier,
        bool fromMigrate,
        bool isViewlessTimeseries) override {
        const auto catalogEntry = getLocalCatalogEntry(opCtx, collectionName);

        // First, validate the test correctly configured whether collections are persisted in
        // the local catalog. If not equal, there's an error with the test.
        ASSERT_EQ(catalogEntry.has_value(), _expectIdentifiers);

        ASSERT_EQ(_expectIdentifiers, collCatalogIdentifier.has_value());

        if (_expectIdentifiers) {
            // Assert 'collCatalogIdentifier' aligns with information persisted in the local
            // catalog entry.
            ASSERT_EQ(catalogEntry->catalogId, collCatalogIdentifier->catalogId);
            ASSERT_EQ(catalogEntry->ident, collCatalogIdentifier->ident);

            if (!idIndex.isEmpty()) {
                // Collection creation included the creation of the '_id_' index.
                ASSERT_TRUE(collCatalogIdentifier->idIndexIdent.has_value());
                const auto expectedIdIndexIdent = catalogEntry->indexIdents.getStringField("_id_");
                ASSERT_EQ(expectedIdIndexIdent, *collCatalogIdentifier->idIndexIdent);
            }
        }
    }

private:
    bool _expectIdentifiers{false};
};

void runCreateCollection(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const CollectionOptions& collectionOptions,
                         bool createDefaultIndexes = true,
                         const BSONObj& idIndex = BSONObj(),
                         bool fromMigrate = false) {
    writeConflictRetry(opCtx, "testCatalogIdentifiers", nss, [&] {
        WriteUnitOfWork wuow(opCtx);
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(opCtx);
        ASSERT_TRUE(db);

        // Signals 'onCreateCollection()' to all OpObservers once complete.
        ASSERT_TRUE(db->createCollection(
            opCtx, nss, collectionOptions, createDefaultIndexes, idIndex, fromMigrate));
        wuow.commit();
    });
};

// Tests 'Database::createCollection()' reports the local catalog identifier for collections
// persisted in the local catalog.
TEST_F(DatabaseTest, CreateCollectionReportsCatalogIdentifier) {
    // OpObserver which validates each collection created in this test is persisted in the local
    // catalog and generates a 'CreateCollCatalogIdentifier'.
    auto createOpObserver =
        std::make_unique<CreateCollCatalogIdentifierObserver>(true /* expectIdentifiers */);
    _opObserverRegistry->addObserver(std::move(createOpObserver));

    auto opCtx = _opCtx.get();

    // Standard replicated collection.
    ASSERT_TRUE(_nss.isReplicated());
    runCreateCollection(opCtx, _nss, CollectionOptions{});

    // Local collection. While local collections aren't replicated, they are still present in
    // the local catalog.
    NamespaceString unreplicatedNSS =
        NamespaceString::createNamespaceString_forTest("local.isUnreplicated");
    ASSERT_FALSE(unreplicatedNSS.isReplicated());
    runCreateCollection(opCtx, unreplicatedNSS, CollectionOptions{});

    // Timeseries buckets collection.
    const NamespaceString bucketsNSS =
        NamespaceString::createNamespaceString_forTest("system.buckets.ts");
    CollectionOptions bucketsOptions{
        .clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat(),
        .timeseries = TimeseriesOptions{"t"}};
    runCreateCollection(opCtx, bucketsNSS, CollectionOptions{}, false /* createDefaultIndexes*/);
}

TEST_F(DatabaseTest, CreateCollectionDoesNotReportCatalogIdentifierForVirtualCollection) {
    RAIIServerParameterControllerForTest replicateLocalCatalogInfoController(
        "featureFlagReplicateLocalCatalogIdentifiers", true);

    // Register an OpObserver to validate the collection isn't persisted in the local catalog and
    // thus does not generate a 'CreateCollCatalogIdentifier'.
    auto createOpObserver =
        std::make_unique<CreateCollCatalogIdentifierObserver>(false /* expectIdentifiers */);
    _opObserverRegistry->addObserver(std::move(createOpObserver));

    // Virtual collections are not persisted to the local catalog.
    VirtualCollectionOptions virtualCollectionOptions;
    const auto url = ExternalDataSourceMetadata::kUrlProtocolFile + "named_pipe1";
    virtualCollectionOptions.dataSources.emplace_back(
        url, StorageTypeEnum::pipe, FileTypeEnum::bson);
    CollectionOptions collectionOptions;
    collectionOptions.setNoIdIndex();

    auto opCtx = _opCtx.get();
    repl::UnreplicatedWritesBlock uwb(opCtx);  // virtual collections are standalone-only
    writeConflictRetry(opCtx, "testNoCatalogIdentifierForVirtualColl", _nss, [&] {
        WriteUnitOfWork wuow(opCtx);
        AutoGetDb autoDb(opCtx, _nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(opCtx);
        ASSERT_TRUE(db);

        // Signals 'onCreateCollection()' to the OpObserver once complete.
        ASSERT_TRUE(
            db->createVirtualCollection(opCtx, _nss, collectionOptions, virtualCollectionOptions));
        wuow.commit();
    });
}

TEST_F(DatabaseTest, CreateCollectionThrowsExceptionWhenDatabaseIsInADropPendingState) {
    writeConflictRetry(
        _opCtx.get(), "testÇreateCollectionWhenDatabaseIsInADropPendingState", _nss, [this] {
            CollectionCatalog::write(_opCtx.get(),
                                     [dbName = _nss.dbName()](CollectionCatalog& catalog) {
                                         catalog.addDropPending(dbName);
                                     });

            AutoGetDb autoDb(_opCtx.get(), _nss.dbName(), MODE_X);
            auto db = autoDb.ensureDbExists(_opCtx.get());
            ASSERT_TRUE(db);

            WriteUnitOfWork wuow(_opCtx.get());

            ASSERT_THROWS_CODE_AND_WHAT(
                db->createCollection(_opCtx.get(), _nss),
                AssertionException,
                ErrorCodes::DatabaseDropPending,
                (StringBuilder() << "Cannot create collection " << _nss.toStringForErrorMsg()
                                 << " - database is in the process of being dropped.")
                    .stringData());
        });
}

void _testDropCollection(OperationContext* opCtx,
                         const NamespaceString& nss,
                         bool createCollectionBeforeDrop,
                         const repl::OpTime& dropOpTime = {},
                         const CollectionOptions& collOpts = {}) {
    if (createCollectionBeforeDrop) {
        writeConflictRetry(opCtx, "testDropCollection", nss, [=] {
            WriteUnitOfWork wuow(opCtx);
            AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
            auto db = autoDb.ensureDbExists(opCtx);
            ASSERT_TRUE(db);
            ASSERT_TRUE(db->createCollection(opCtx, nss, collOpts));
            wuow.commit();
        });
    }

    writeConflictRetry(opCtx, "testDropCollection", nss, [=] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(opCtx);
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(opCtx);
        if (!createCollectionBeforeDrop) {
            ASSERT_FALSE(CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));
        }

        ASSERT_OK(db->dropCollection(opCtx, nss, dropOpTime));
        ASSERT_FALSE(CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));
        wuow.commit();
    });
}

TEST_F(DatabaseTest, DropCollectionReturnsOKIfCollectionDoesNotExist) {
    _testDropCollection(_opCtx.get(), _nss, false);
    // Check last optime for this client to ensure no entries were appended to the oplog.
    ASSERT_EQUALS(repl::OpTime(), repl::ReplClientInfo::forClient(&cc()).getLastOp());
}

TEST_F(DatabaseTest, DropCollectionDropsCollectionButDoesNotLogOperationIfWritesAreNotReplicated) {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    ASSERT_FALSE(_opCtx->writesAreReplicated());
    ASSERT_TRUE(
        repl::ReplicationCoordinator::get(_opCtx.get())->isOplogDisabledFor(_opCtx.get(), _nss));

    _testDropCollection(_opCtx.get(), _nss, true);

    // Drop optime is null because no op was written to the oplog.
    auto dropOpTime = repl::ReplClientInfo::forClient(&cc()).getLastOp();
    ASSERT_EQUALS(repl::OpTime(), dropOpTime);
}

TEST_F(DatabaseTest, DropCollectionRejectsProvidedDropOpTimeIfWritesAreReplicated) {
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(
        repl::ReplicationCoordinator::get(_opCtx.get())->isOplogDisabledFor(_opCtx.get(), _nss));

    auto opCtx = _opCtx.get();
    auto nss = _nss;
    AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
    auto db = autoDb.ensureDbExists(opCtx);
    writeConflictRetry(opCtx, "testDropOpTimeWithReplicated", nss, [&] {
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(opCtx);
        ASSERT_TRUE(db->createCollection(opCtx, nss));
        wuow.commit();
    });

    WriteUnitOfWork wuow(opCtx);
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    ASSERT_EQUALS(ErrorCodes::BadValue, db->dropCollection(opCtx, nss, dropOpTime));
}

void _testDropCollectionThrowsExceptionIfThereAreIndexesInProgress(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    writeConflictRetry(opCtx, "testDropCollectionWithIndexesInProgress", nss, [opCtx, nss] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(opCtx);
        ASSERT_TRUE(db);

        Collection* collection = nullptr;
        {
            WriteUnitOfWork wuow(opCtx);
            ASSERT_TRUE((collection = db->createCollection(opCtx, nss)));
            wuow.commit();
        }

        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT_EQUALS(indexCatalog->numIndexesInProgress(), 0);
        auto indexInfoObj =
            BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("a" << 1) << "name"
                     << "a_1");

        IndexBuildBlock indexBuildBlock(
            collection->ns(), indexInfoObj, IndexBuildMethodEnum::kHybrid, UUID::gen());
        {
            WriteUnitOfWork wuow(opCtx);
            IndexBuildInfo indexBuildInfo(indexInfoObj,
                                          *storageEngine,
                                          collection->ns().dbName(),
                                          VersionContext::getDecoration(opCtx));
            ASSERT_OK(
                indexBuildBlock.init(opCtx, collection, indexBuildInfo, /*forRecovery=*/false));
            wuow.commit();
        }
        ON_BLOCK_EXIT([&indexBuildBlock, opCtx, collection] {
            WriteUnitOfWork wuow(opCtx);
            indexBuildBlock.success(opCtx, collection);
            wuow.commit();
        });

        ASSERT_GREATER_THAN(indexCatalog->numIndexesInProgress(), 0);

        WriteUnitOfWork wuow(opCtx);
        ASSERT_THROWS_CODE(db->dropCollection(opCtx, nss),
                           AssertionException,
                           ErrorCodes::BackgroundOperationInProgressForNamespace);
    });
}

TEST_F(DatabaseTest,
       DropCollectionThrowsExceptionIfThereAreIndexesInProgressAndWritesAreNotReplicated) {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    ASSERT_FALSE(_opCtx->writesAreReplicated());
    _testDropCollectionThrowsExceptionIfThereAreIndexesInProgress(_opCtx.get(), _nss);
}

TEST_F(DatabaseTest,
       DropCollectionThrowsExceptionIfThereAreIndexesInProgressAndWritesAreReplicated) {
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    _testDropCollectionThrowsExceptionIfThereAreIndexesInProgress(_opCtx.get(), _nss);
}

TEST_F(DatabaseTest, RenameCollectionPreservesUuidOfSourceCollectionAndUpdatesUuidCatalog) {
    auto opCtx = _opCtx.get();
    auto fromNss = _nss;
    auto toNss = NamespaceString::createNamespaceString_forTest(fromNss.getSisterNS("bar"));
    ASSERT_NOT_EQUALS(fromNss, toNss);

    AutoGetDb autoDb(opCtx, fromNss.dbName(), MODE_X);
    auto db = autoDb.ensureDbExists(opCtx);
    ASSERT_TRUE(db);

    auto fromUuid = UUID::gen();
    writeConflictRetry(opCtx, "create", fromNss, [&] {
        auto catalog = CollectionCatalog::get(opCtx);
        ASSERT_EQUALS(boost::none, catalog->lookupNSSByUUID(opCtx, fromUuid));

        WriteUnitOfWork wuow(opCtx);
        CollectionOptions fromCollectionOptions;
        fromCollectionOptions.uuid = fromUuid;
        ASSERT_TRUE(db->createCollection(opCtx, fromNss, fromCollectionOptions));
        ASSERT_EQUALS(fromNss, *catalog->lookupNSSByUUID(opCtx, fromUuid));
        wuow.commit();
    });

    writeConflictRetry(opCtx, "rename", fromNss, [&] {
        WriteUnitOfWork wuow(opCtx);
        auto stayTemp = false;
        ASSERT_OK(db->renameCollection(opCtx, fromNss, toNss, stayTemp));

        auto catalog = CollectionCatalog::get(opCtx);
        ASSERT_FALSE(catalog->lookupCollectionByNamespace(opCtx, fromNss));
        auto toCollection = catalog->lookupCollectionByNamespace(opCtx, toNss);
        ASSERT_TRUE(toCollection);

        const auto& toCollectionOptions = toCollection->getCollectionOptions();

        auto toUuid = toCollectionOptions.uuid;
        ASSERT_TRUE(toUuid);
        ASSERT_EQUALS(fromUuid, *toUuid);

        ASSERT_EQUALS(toNss, *catalog->lookupNSSByUUID(opCtx, *toUuid));

        wuow.commit();
    });
}

TEST_F(DatabaseTest,
       MakeUniqueCollectionNamespaceReturnsFailedToParseIfModelDoesNotContainPercentSign) {
    writeConflictRetry(_opCtx.get(), "testMakeUniqueCollectionNamespace", _nss, [this] {
        AutoGetDb autoDb(_opCtx.get(), _nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(_opCtx.get());
        ASSERT_TRUE(db);
        ASSERT_EQUALS(ErrorCodes::FailedToParse,
                      makeUniqueCollectionName(
                          _opCtx.get(), db->name(), "CollectionModelWithoutPercentSign"));
    });
}

TEST_F(DatabaseTest, MakeUniqueCollectionNamespaceReplacesPercentSignsWithRandomCharacters) {
    writeConflictRetry(_opCtx.get(), "testMakeUniqueCollectionNamespace", _nss, [this] {
        AutoGetDb autoDb(_opCtx.get(), _nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(_opCtx.get());
        ASSERT_TRUE(db);

        auto model = "tmp%%%%"_sd;
        pcre::Regex re(_nss.db_forTest() + "\\.tmp[0-9A-Za-z][0-9A-Za-z][0-9A-Za-z][0-9A-Za-z]",
                       pcre::ANCHORED | pcre::ENDANCHORED);

        auto nss1 = unittest::assertGet(makeUniqueCollectionName(_opCtx.get(), db->name(), model));
        if (!re.matchView(nss1.ns_forTest())) {
            FAIL((StringBuilder() << "First generated namespace \"" << nss1.ns_forTest()
                                  << "\" does not match regular expression \"" << re.pattern()
                                  << "\"")
                     .str());
        }

        // Create collection using generated namespace so that makeUniqueCollectionNamespace()
        // will not return the same namespace the next time. This is because we check the
        // existing collections in the database for collisions while generating the namespace.
        {
            WriteUnitOfWork wuow(_opCtx.get());
            ASSERT_TRUE(db->createCollection(_opCtx.get(), nss1));
            wuow.commit();
        }

        auto nss2 = unittest::assertGet(makeUniqueCollectionName(_opCtx.get(), db->name(), model));
        if (!re.matchView(nss2.ns_forTest())) {
            FAIL((StringBuilder() << "Second generated namespace \"" << nss2.ns_forTest()
                                  << "\" does not match regular expression \"" << re.pattern()
                                  << "\"")
                     .str());
        }

        // Second generated namespace should not collide with the first because a collection
        // with name matching nss1 now exists.
        ASSERT_NOT_EQUALS(nss1, nss2);
    });
}

TEST_F(
    DatabaseTest,
    MakeUniqueCollectionNamespaceReturnsNamespaceExistsIfGeneratedNamesMatchExistingCollections) {
    writeConflictRetry(_opCtx.get(), "testMakeUniqueCollectionNamespace", _nss, [this] {
        AutoGetDb autoDb(_opCtx.get(), _nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(_opCtx.get());
        ASSERT_TRUE(db);

        auto model = "tmp%"_sd;

        // Create all possible collections matching model with single percent sign.
        const auto charsToChooseFrom =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"_sd;
        for (const auto c : charsToChooseFrom) {
            NamespaceString nss = NamespaceString::createNamespaceString_forTest(
                _nss.dbName(), model.substr(0, model.find('%')) + std::string(1U, c));
            WriteUnitOfWork wuow(_opCtx.get());
            ASSERT_TRUE(db->createCollection(_opCtx.get(), nss));
            wuow.commit();
        }

        // makeUniqueCollectionName() returns NamespaceExists because it will not be able to
        // generate a namespace that will not collide with an existings collection.
        ASSERT_EQUALS(ErrorCodes::NamespaceExists,
                      makeUniqueCollectionName(_opCtx.get(), db->name(), model));
    });
}

TEST_F(DatabaseTest, AutoGetDBSucceedsWithDeadlineNow) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    Lock::DBLock lock(_opCtx.get(), nss.dbName(), MODE_X);
    ASSERT(shard_role_details::getLocker(_opCtx.get())->isDbLockedForMode(nss.dbName(), MODE_X));
    try {
        AutoGetDb db(_opCtx.get(), nss.dbName(), MODE_X, Date_t::now());
        ASSERT(
            shard_role_details::getLocker(_opCtx.get())->isDbLockedForMode(nss.dbName(), MODE_X));
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
        FAIL("Should get the db within the timeout");
    }
}

TEST_F(DatabaseTest, AutoGetDBSucceedsWithDeadlineMin) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    Lock::DBLock lock(_opCtx.get(), nss.dbName(), MODE_X);
    ASSERT(shard_role_details::getLocker(_opCtx.get())->isDbLockedForMode(nss.dbName(), MODE_X));
    try {
        AutoGetDb db(_opCtx.get(), nss.dbName(), MODE_X, Date_t());
        ASSERT(
            shard_role_details::getLocker(_opCtx.get())->isDbLockedForMode(nss.dbName(), MODE_X));
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
        FAIL("Should get the db within the timeout");
    }
}

TEST_F(DatabaseTest, AutoGetCollectionForReadCommandSucceedsWithDeadlineNow) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    Lock::DBLock dbLock(_opCtx.get(), nss.dbName(), MODE_X);
    ASSERT(shard_role_details::getLocker(_opCtx.get())->isDbLockedForMode(nss.dbName(), MODE_X));
    Lock::CollectionLock collLock(_opCtx.get(), nss, MODE_X);
    ASSERT(shard_role_details::getLocker(_opCtx.get())->isCollectionLockedForMode(nss, MODE_X));
    try {
        AutoGetCollectionForReadCommand db(
            _opCtx.get(), nss, AutoGetCollection::Options{}.deadline(Date_t::now()));
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
        FAIL("Should get the db within the timeout");
    }
}

TEST_F(DatabaseTest, AutoGetCollectionForReadCommandSucceedsWithDeadlineMin) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    Lock::DBLock dbLock(_opCtx.get(), nss.dbName(), MODE_X);
    ASSERT(shard_role_details::getLocker(_opCtx.get())->isDbLockedForMode(nss.dbName(), MODE_X));
    Lock::CollectionLock collLock(_opCtx.get(), nss, MODE_X);
    ASSERT(shard_role_details::getLocker(_opCtx.get())->isCollectionLockedForMode(nss, MODE_X));
    try {
        AutoGetCollectionForReadCommand db(
            _opCtx.get(), nss, AutoGetCollection::Options{}.deadline(Date_t()));
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
        FAIL("Should get the db within the timeout");
    }
}

TEST_F(DatabaseTest, CreateCollectionProhibitsReplicatedCollectionsWithoutIdIndex) {
    writeConflictRetry(_opCtx.get(),
                       "testÇreateCollectionProhibitsReplicatedCollectionsWithoutIdIndex",
                       _nss,
                       [this] {
                           AutoGetDb autoDb(_opCtx.get(), _nss.dbName(), MODE_X);
                           auto db = autoDb.ensureDbExists(_opCtx.get());
                           ASSERT_TRUE(db);

                           WriteUnitOfWork wuow(_opCtx.get());

                           CollectionOptions options;
                           options.setNoIdIndex();

                           ASSERT_THROWS_CODE_AND_WHAT(
                               db->createCollection(_opCtx.get(), _nss, options),
                               AssertionException,
                               50001,
                               (StringBuilder()
                                << "autoIndexId:false is not allowed for collection "
                                << _nss.toStringForErrorMsg() << " because it can be replicated")
                                   .stringData());
                       });
}


TEST_F(DatabaseTest, DatabaseHolderImplTest) {
    DatabaseHolderImpl::DBsIndex dbIndex;
    ASSERT_EQUALS(dbIndex.viewAll().size(), 0);

    auto insertTest = [&dbIndex](const DatabaseName& dbName, bool insertNullFirst) {
        if (insertNullFirst) {
            dbIndex.getOrCreate(dbName);  // <dbName> -> nullptr
            ASSERT_EQUALS(dbIndex.viewAll().find(dbName)->second, nullptr);
        }

        auto p = dbIndex.upsert(dbName, std::make_unique<DatabaseImpl>(dbName));
        Database* db = p.first;
        ASSERT_EQUALS(p.second, !insertNullFirst);
        ASSERT_NOT_EQUALS(db, nullptr);
        ASSERT_NOT_EQUALS(dbIndex.viewAll().find(dbName)->second, nullptr);
    };

    DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(boost::none, "foo1");
    TenantId tenant2 = TenantId(OID::gen());
    DatabaseName dbName2 = DatabaseName::createDatabaseName_forTest(tenant2, "foo2");
    insertTest(dbName1, true);
    insertTest(dbName2, false);

    auto conflict = dbIndex.getAnyConflictingName(
        DatabaseName::createDatabaseName_forTest(boost::none, "foo99"));
    ASSERT_FALSE(conflict);

    conflict = dbIndex.getAnyConflictingName(
        DatabaseName::createDatabaseName_forTest(boost::none, "foo1"));
    ASSERT_FALSE(conflict);  // No self conflict
    conflict =
        dbIndex.getAnyConflictingName(DatabaseName::createDatabaseName_forTest(tenant2, "foo2"));
    ASSERT_FALSE(conflict);  // No self conflict

    conflict = dbIndex.getAnyConflictingName(
        DatabaseName::createDatabaseName_forTest(boost::none, "fOO1"));
    ASSERT_TRUE(conflict);
    ASSERT_EQUALS(*conflict, dbName1);


    conflict = dbIndex.getAnyConflictingName(
        DatabaseName::createDatabaseName_forTest(boost::none, "foo2"));
    ASSERT_FALSE(conflict);  // No conflict different tenant

    conflict = dbIndex.getAnyConflictingName(
        DatabaseName::createDatabaseName_forTest(boost::none, "fOO2"));
    ASSERT_FALSE(conflict);  // No conflict different tenant

    conflict =
        dbIndex.getAnyConflictingName(DatabaseName::createDatabaseName_forTest(tenant2, "FOO2"));
    ASSERT_TRUE(conflict);
    ASSERT_EQUALS(*conflict, dbName2);

    ASSERT_EQUALS(dbIndex.viewAll().size(), 2);
    dbIndex.erase(dbName2);
    ASSERT_EQUALS(dbIndex.viewAll().size(), 1);
}

}  // namespace
}  // namespace mongo

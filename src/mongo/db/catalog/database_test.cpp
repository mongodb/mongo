/**
 *    Copyright 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <pcrecpp.h>

#include "mongo/bson/util/builder.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace {

using namespace mongo;

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

class DatabaseTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    NamespaceString _nss;
};

void DatabaseTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    _opCtx = cc().makeOperationContext();

    repl::StorageInterface::set(service, stdx::make_unique<repl::StorageInterfaceMock>());
    repl::DropPendingCollectionReaper::set(
        service,
        stdx::make_unique<repl::DropPendingCollectionReaper>(repl::StorageInterface::get(service)));

    // Set up ReplicationCoordinator and create oplog.
    repl::ReplicationCoordinator::set(service,
                                      stdx::make_unique<repl::ReplicationCoordinatorMock>(service));
    repl::setOplogCollectionName();
    repl::createOplog(_opCtx.get());

    // Ensure that we are primary.
    auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Set up OpObserver so that Database will append actual oplog entries to the oplog using
    // repl::logOp(). repl::logOp() will also store the oplog entry's optime in ReplClientInfo.
    service->setOpObserver(stdx::make_unique<OpObserverImpl>());

    _nss = NamespaceString("test.foo");
}

void DatabaseTest::tearDown() {
    _nss = {};
    _opCtx = {};

    auto service = getServiceContext();
    repl::DropPendingCollectionReaper::set(service, {});
    repl::StorageInterface::set(service, {});

    ServiceContextMongoDTest::tearDown();
}

TEST_F(DatabaseTest, SetDropPendingThrowsExceptionIfDatabaseIsAlreadyInADropPendingState) {
    writeConflictRetry(_opCtx.get(), "testSetDropPending", _nss.ns(), [this] {
        AutoGetOrCreateDb autoDb(_opCtx.get(), _nss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        ASSERT_FALSE(db->isDropPending(_opCtx.get()));
        db->setDropPending(_opCtx.get(), true);
        ASSERT_TRUE(db->isDropPending(_opCtx.get()));

        ASSERT_THROWS_CODE_AND_WHAT(
            db->setDropPending(_opCtx.get(), true),
            AssertionException,
            ErrorCodes::DatabaseDropPending,
            (StringBuilder() << "Unable to drop database " << _nss.db()
                             << " because it is already in the process of being dropped.")
                .stringData());

        db->setDropPending(_opCtx.get(), false);
        ASSERT_FALSE(db->isDropPending(_opCtx.get()));

        // It's fine to reset 'dropPending' multiple times.
        db->setDropPending(_opCtx.get(), false);
        ASSERT_FALSE(db->isDropPending(_opCtx.get()));
    });
}

TEST_F(DatabaseTest, CreateCollectionThrowsExceptionWhenDatabaseIsInADropPendingState) {
    writeConflictRetry(
        _opCtx.get(), "testÇreateCollectionWhenDatabaseIsInADropPendingState", _nss.ns(), [this] {
            AutoGetOrCreateDb autoDb(_opCtx.get(), _nss.db(), MODE_X);
            auto db = autoDb.getDb();
            ASSERT_TRUE(db);

            db->setDropPending(_opCtx.get(), true);

            WriteUnitOfWork wuow(_opCtx.get());

            // If createCollection() unexpectedly succeeds, we need to commit the collection
            // creation to
            // avoid leaving the ephemeralForTest storage engine in a bad state for subsequent
            // tests.
            ON_BLOCK_EXIT([&wuow] { wuow.commit(); });

            ASSERT_THROWS_CODE_AND_WHAT(
                db->createCollection(_opCtx.get(), _nss.ns()),
                AssertionException,
                ErrorCodes::DatabaseDropPending,
                (StringBuilder() << "Cannot create collection " << _nss.ns()
                                 << " - database is in the process of being dropped.")
                    .stringData());
        });
}

void _testDropCollection(OperationContext* opCtx,
                         const NamespaceString& nss,
                         bool createCollectionBeforeDrop,
                         const repl::OpTime& dropOpTime = {},
                         const CollectionOptions& collOpts = {}) {
    writeConflictRetry(opCtx, "testDropCollection", nss.ns(), [=] {
        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(opCtx);
        if (createCollectionBeforeDrop) {
            ASSERT_TRUE(db->createCollection(opCtx, nss.ns(), collOpts));
        } else {
            ASSERT_FALSE(db->getCollection(opCtx, nss));
        }

        ASSERT_OK(db->dropCollection(opCtx, nss.ns(), dropOpTime));

        ASSERT_FALSE(db->getCollection(opCtx, nss));
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

TEST_F(DatabaseTest,
       DropCollectionRenamesCollectionToPendingDropNamespaceAndLogsOperationIfWritesAreReplicated) {
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(
        repl::ReplicationCoordinator::get(_opCtx.get())->isOplogDisabledFor(_opCtx.get(), _nss));

    _testDropCollection(_opCtx.get(), _nss, true);

    // Drop optime is non-null because an op was written to the oplog.
    auto dropOpTime = repl::ReplClientInfo::forClient(&cc()).getLastOp();
    ASSERT_GREATER_THAN(dropOpTime, repl::OpTime());

    // Replicated collection is renamed with a special drop-pending names in the <db>.system.drop.*
    // namespace.
    auto dpns = _nss.makeDropPendingNamespace(dropOpTime);
    ASSERT_TRUE(mongo::AutoGetCollectionForRead(_opCtx.get(), dpns).getCollection());

    // Reaper should have the drop optime of the collection.
    auto reaperEarliestDropOpTime =
        repl::DropPendingCollectionReaper::get(_opCtx.get())->getEarliestDropOpTime();
    ASSERT_TRUE(reaperEarliestDropOpTime);
    ASSERT_EQUALS(dropOpTime, *reaperEarliestDropOpTime);
}

/**
 * Sets up ReplicationCoordinator for master/slave.
 */
void _setUpMasterSlave(ServiceContext* service) {
    repl::ReplSettings settings;
    settings.setOplogSizeBytes(10 * 1024 * 1024);
    settings.setMaster(true);
    repl::ReplicationCoordinator::set(
        service, stdx::make_unique<repl::ReplicationCoordinatorMock>(service, settings));
    auto replCoord = repl::ReplicationCoordinator::get(service);
    ASSERT_TRUE(repl::ReplicationCoordinator::modeMasterSlave == replCoord->getReplicationMode());
}

TEST_F(DatabaseTest,
       DropCollectionDropsCollectionAndLogsOperationIfWritesAreReplicatedAndReplModeIsMasterSlave) {
    _setUpMasterSlave(getServiceContext());

    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(
        repl::ReplicationCoordinator::get(_opCtx.get())->isOplogDisabledFor(_opCtx.get(), _nss));

    _testDropCollection(_opCtx.get(), _nss, true);

    // Drop optime is non-null because an op was written to the oplog.
    auto dropOpTime = repl::ReplClientInfo::forClient(&cc()).getLastOp();
    ASSERT_GREATER_THAN(dropOpTime, repl::OpTime());

    // Replicated collection should not be renamed under master/slave.
    auto dpns = _nss.makeDropPendingNamespace(dropOpTime);
    ASSERT_FALSE(mongo::AutoGetCollectionForRead(_opCtx.get(), dpns).getCollection());

    // Reaper should not have the drop optime of the collection.
    auto reaperEarliestDropOpTime =
        repl::DropPendingCollectionReaper::get(_opCtx.get())->getEarliestDropOpTime();
    ASSERT_FALSE(reaperEarliestDropOpTime);
}

TEST_F(DatabaseTest, DropCollectionRejectsProvidedDropOpTimeIfWritesAreReplicated) {
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(
        repl::ReplicationCoordinator::get(_opCtx.get())->isOplogDisabledFor(_opCtx.get(), _nss));

    auto opCtx = _opCtx.get();
    auto nss = _nss;
    writeConflictRetry(opCtx, "testDropOpTimeWithReplicated", nss.ns(), [opCtx, nss] {
        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(opCtx);
        ASSERT_TRUE(db->createCollection(opCtx, nss.ns()));

        repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
        ASSERT_EQUALS(ErrorCodes::BadValue, db->dropCollection(opCtx, nss.ns(), dropOpTime));
    });
}

TEST_F(
    DatabaseTest,
    DropCollectionRenamesCollectionToPendingDropNamespaceUsingProvidedDropOpTimeButDoesNotLogOperation) {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    ASSERT_FALSE(_opCtx->writesAreReplicated());
    ASSERT_TRUE(
        repl::ReplicationCoordinator::get(_opCtx.get())->isOplogDisabledFor(_opCtx.get(), _nss));

    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    _testDropCollection(_opCtx.get(), _nss, true, dropOpTime);

    // Last optime in repl client is null because we did not write to the oplog.
    ASSERT_EQUALS(repl::OpTime(), repl::ReplClientInfo::forClient(&cc()).getLastOp());

    // Replicated collection is renamed with a special drop-pending names in the <db>.system.drop.*
    // namespace.
    auto dpns = _nss.makeDropPendingNamespace(dropOpTime);
    ASSERT_TRUE(mongo::AutoGetCollectionForRead(_opCtx.get(), dpns).getCollection());

    // Reaper should have the drop optime of the collection.
    auto reaperEarliestDropOpTime =
        repl::DropPendingCollectionReaper::get(_opCtx.get())->getEarliestDropOpTime();
    ASSERT_TRUE(reaperEarliestDropOpTime);
    ASSERT_EQUALS(dropOpTime, *reaperEarliestDropOpTime);
}

TEST_F(
    DatabaseTest,
    DropCollectionIgnoresProvidedDropOpTimeAndDropsCollectionButDoesNotLogOperationIfReplModeIsMasterSlave) {
    _setUpMasterSlave(getServiceContext());

    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    ASSERT_FALSE(_opCtx->writesAreReplicated());
    ASSERT_TRUE(
        repl::ReplicationCoordinator::get(_opCtx.get())->isOplogDisabledFor(_opCtx.get(), _nss));

    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    _testDropCollection(_opCtx.get(), _nss, true, dropOpTime);

    // Last optime in repl client is null because we did not write to the oplog.
    ASSERT_EQUALS(repl::OpTime(), repl::ReplClientInfo::forClient(&cc()).getLastOp());

    // Collection is not renamed under master/slave.
    auto dpns = _nss.makeDropPendingNamespace(dropOpTime);
    ASSERT_FALSE(mongo::AutoGetCollectionForRead(_opCtx.get(), dpns).getCollection());

    // Reaper should not have the drop optime of the collection.
    auto reaperEarliestDropOpTime =
        repl::DropPendingCollectionReaper::get(_opCtx.get())->getEarliestDropOpTime();
    ASSERT_FALSE(reaperEarliestDropOpTime);
}

TEST_F(DatabaseTest, DropPendingCollectionIsAllowedToHaveDocumentValidators) {
    CollectionOptions opts;
    opts.validator = BSON("x" << BSON("$type"
                                      << "string"));
    opts.validationAction = "error";
    _testDropCollection(_opCtx.get(), _nss, true, {}, opts);
}

void _testDropCollectionThrowsExceptionIfThereAreIndexesInProgress(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    writeConflictRetry(opCtx, "testDropCollectionWithIndexesInProgress", nss.ns(), [opCtx, nss] {
        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        Collection* collection = nullptr;
        {
            WriteUnitOfWork wuow(opCtx);
            ASSERT_TRUE(collection = db->createCollection(opCtx, nss.ns()));
            wuow.commit();
        }

        MultiIndexBlock indexer(opCtx, collection);
        ON_BLOCK_EXIT([&indexer, opCtx] {
            WriteUnitOfWork wuow(opCtx);
            indexer.commit();
            wuow.commit();
        });

        auto indexCatalog = collection->getIndexCatalog();
        ASSERT_EQUALS(indexCatalog->numIndexesInProgress(opCtx), 0);
        auto indexInfoObj = BSON(
            "v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << BSON("a" << 1) << "name"
                << "a_1"
                << "ns"
                << nss.ns());
        ASSERT_OK(indexer.init(indexInfoObj).getStatus());
        ASSERT_GREATER_THAN(indexCatalog->numIndexesInProgress(opCtx), 0);

        WriteUnitOfWork wuow(opCtx);
        ASSERT_THROWS_CODE(
            db->dropCollection(opCtx, nss.ns()).transitional_ignore(), AssertionException, 40461);
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
    auto toNss = NamespaceString(fromNss.getSisterNS("bar"));
    ASSERT_NOT_EQUALS(fromNss, toNss);

    writeConflictRetry(opCtx, "testRenameCollection", fromNss.ns(), [=] {
        AutoGetOrCreateDb autoDb(opCtx, fromNss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        auto fromUuid = UUID::gen();

        auto&& uuidCatalog = UUIDCatalog::get(opCtx);
        ASSERT_EQUALS(NamespaceString(), uuidCatalog.lookupNSSByUUID(fromUuid));

        WriteUnitOfWork wuow(opCtx);
        CollectionOptions fromCollectionOptions;
        fromCollectionOptions.uuid = fromUuid;
        ASSERT_TRUE(db->createCollection(opCtx, fromNss.ns(), fromCollectionOptions));
        ASSERT_EQUALS(fromNss, uuidCatalog.lookupNSSByUUID(fromUuid));

        auto stayTemp = false;
        ASSERT_OK(db->renameCollection(opCtx, fromNss.ns(), toNss.ns(), stayTemp));

        ASSERT_FALSE(db->getCollection(opCtx, fromNss));
        auto toCollection = db->getCollection(opCtx, toNss);
        ASSERT_TRUE(toCollection);

        auto catalogEntry = toCollection->getCatalogEntry();
        auto toCollectionOptions = catalogEntry->getCollectionOptions(opCtx);

        auto toUuid = toCollectionOptions.uuid;
        ASSERT_TRUE(toUuid);
        ASSERT_EQUALS(fromUuid, *toUuid);

        ASSERT_EQUALS(toNss, uuidCatalog.lookupNSSByUUID(*toUuid));

        wuow.commit();
    });
}

TEST_F(DatabaseTest,
       MakeUniqueCollectionNamespaceReturnsFailedToParseIfModelDoesNotContainPercentSign) {
    writeConflictRetry(_opCtx.get(), "testMakeUniqueCollectionNamespace", _nss.ns(), [this] {
        AutoGetOrCreateDb autoDb(_opCtx.get(), _nss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);
        ASSERT_EQUALS(
            ErrorCodes::FailedToParse,
            db->makeUniqueCollectionNamespace(_opCtx.get(), "CollectionModelWithoutPercentSign"));

        // Generated namespace has to satisfy namespace length constraints so we will reject
        // any collection model where the first substituted percent sign will not be in the
        // generated namespace. See NamespaceString::MaxNsCollectionLen.
        auto dbPrefix = _nss.db() + ".";
        auto modelTooLong =
            (StringBuilder() << dbPrefix
                             << std::string('x',
                                            NamespaceString::MaxNsCollectionLen - dbPrefix.size())
                             << "%")
                .str();
        ASSERT_EQUALS(ErrorCodes::FailedToParse,
                      db->makeUniqueCollectionNamespace(_opCtx.get(), modelTooLong));
    });
}

TEST_F(DatabaseTest, MakeUniqueCollectionNamespaceReplacesPercentSignsWithRandomCharacters) {
    writeConflictRetry(_opCtx.get(), "testMakeUniqueCollectionNamespace", _nss.ns(), [this] {
        AutoGetOrCreateDb autoDb(_opCtx.get(), _nss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        auto model = "tmp%%%%"_sd;
        pcrecpp::RE re(_nss.db() + "\\.tmp[0-9A-Za-z][0-9A-Za-z][0-9A-Za-z][0-9A-Za-z]");

        auto nss1 = unittest::assertGet(db->makeUniqueCollectionNamespace(_opCtx.get(), model));
        if (!re.FullMatch(nss1.ns())) {
            FAIL((StringBuilder() << "First generated namespace \"" << nss1.ns()
                                  << "\" does not match reqular expression \""
                                  << re.pattern()
                                  << "\"")
                     .str());
        }

        // Create collection using generated namespace so that makeUniqueCollectionNamespace() will
        // not return the same namespace the next time. This is because we check the existing
        // collections in the database for collisions while generating the namespace.
        {
            WriteUnitOfWork wuow(_opCtx.get());
            ASSERT_TRUE(db->createCollection(_opCtx.get(), nss1.ns()));
            wuow.commit();
        }

        auto nss2 = unittest::assertGet(db->makeUniqueCollectionNamespace(_opCtx.get(), model));
        if (!re.FullMatch(nss2.ns())) {
            FAIL((StringBuilder() << "Second generated namespace \"" << nss2.ns()
                                  << "\" does not match reqular expression \""
                                  << re.pattern()
                                  << "\"")
                     .str());
        }

        // Second generated namespace should not collide with the first because a collection with
        // name matching nss1 now exists.
        ASSERT_NOT_EQUALS(nss1, nss2);
    });
}

TEST_F(
    DatabaseTest,
    MakeUniqueCollectionNamespaceReturnsNamespaceExistsIfGeneratedNamesMatchExistingCollections) {
    writeConflictRetry(_opCtx.get(), "testMakeUniqueCollectionNamespace", _nss.ns(), [this] {
        AutoGetOrCreateDb autoDb(_opCtx.get(), _nss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        auto model = "tmp%"_sd;

        // Create all possible collections matching model with single percent sign.
        const auto charsToChooseFrom =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"_sd;
        for (const auto c : charsToChooseFrom) {
            NamespaceString nss(_nss.db(), model.substr(0, model.find('%')) + std::string(1U, c));
            WriteUnitOfWork wuow(_opCtx.get());
            ASSERT_TRUE(db->createCollection(_opCtx.get(), nss.ns()));
            wuow.commit();
        }

        // makeUniqueCollectionNamespace() returns NamespaceExists because it will not be able to
        // generate a namespace that will not collide with an existings collection.
        ASSERT_EQUALS(ErrorCodes::NamespaceExists,
                      db->makeUniqueCollectionNamespace(_opCtx.get(), model));
    });
}

TEST_F(DatabaseTest, DBLockCanBePassedToAutoGetDb) {
    NamespaceString nss("test", "coll");
    Lock::DBLock lock(_opCtx.get(), nss.db(), MODE_X);
    {
        AutoGetDb db(_opCtx.get(), nss.db(), std::move(lock));
        ASSERT(_opCtx.get()->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    }
    // The moved lock should go out of scope here, so the database should no longer be locked.
    ASSERT_FALSE(_opCtx.get()->lockState()->isDbLockedForMode(nss.db(), MODE_X));
}

TEST_F(DatabaseTest, DBLockCanBePassedToAutoGetCollectionOrViewForReadCommand) {
    NamespaceString nss("test", "coll");
    Lock::DBLock lock(_opCtx.get(), nss.db(), MODE_X);
    {
        AutoGetCollectionOrViewForReadCommand coll(_opCtx.get(), nss, std::move(lock));
        ASSERT(_opCtx.get()->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    }
    // The moved lock should go out of scope here, so the database should no longer be locked.
    ASSERT_FALSE(_opCtx.get()->lockState()->isDbLockedForMode(nss.db(), MODE_X));
}
}  // namespace

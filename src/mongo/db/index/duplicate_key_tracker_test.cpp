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

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/duplicate_key_tracker.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

static const StringData kConstraintNsPrefix = "local.system.indexBuildConstraints"_sd;

class DuplicateKeyTrackerTest : public ServiceContextMongoDTest {
public:
    DuplicateKeyTrackerTest()
        : ServiceContextMongoDTest("ephemeralForTest"), _opCtx(cc().makeOperationContext()) {
        repl::ReplicationCoordinator::set(
            getServiceContext(),
            stdx::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext()));
    }

    std::unique_ptr<DuplicateKeyTracker> makeTracker(OperationContext* opCtx,
                                                     const IndexCatalogEntry* entry) {
        NamespaceString tempNss = DuplicateKeyTracker::makeTempNamespace();
        makeTempCollection(opCtx, tempNss);

        auto tracker = std::make_unique<DuplicateKeyTracker>(entry, tempNss);

        AutoGetCollection autoColl(opCtx, tracker->nss(), MODE_IS);
        ASSERT(autoColl.getCollection());

        return tracker;
    };

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    void tearDown() {}

    void makeTempCollection(OperationContext* opCtx, const NamespaceString& nss) {
        WriteUnitOfWork wuow(opCtx);

        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_X);
        auto db = autoDb.getDb();
        invariant(db);

        ASSERT(!db->getCollection(opCtx, nss));

        CollectionOptions options;
        options.setNoIdIndex();

        // Create the temp collection
        auto coll = db->createCollection(opCtx, nss.ns(), options);
        invariant(coll);
        wuow.commit();
    }

    void destroyTempCollection(OperationContext* opCtx, const NamespaceString& nss) {
        WriteUnitOfWork wuow(opCtx);

        AutoGetDb autoDb(opCtx, nss.db(), MODE_X);
        auto db = autoDb.getDb();
        invariant(db);

        ASSERT_OK(db->dropCollectionEvenIfSystem(opCtx, nss));
        wuow.commit();
    }

private:
    int _numIndexesCreated = 0;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(DuplicateKeyTrackerTest, IndexBuild) {
    std::unique_ptr<DuplicateKeyTracker> tracker;

    const NamespaceString collNs("test.myCollection");
    const BSONObj doc1 = BSON("_id" << 1 << "a" << 1);
    const BSONObj doc2 = BSON("_id" << 2 << "a" << 1);

    // Create the collection with a two documents that have the same key for 'a'.
    {
        AutoGetOrCreateDb dbRaii(opCtx(), collNs.db(), LockMode::MODE_X);
        WriteUnitOfWork wunit(opCtx());
        CollectionOptions options;
        options.uuid = UUID::gen();
        auto coll = dbRaii.getDb()->createCollection(opCtx(), collNs.ns(), options);
        ASSERT(coll);

        ASSERT_OK(coll->insertDocument(opCtx(), InsertStatement(doc1), nullptr));
        ASSERT_OK(coll->insertDocument(opCtx(), InsertStatement(doc2), nullptr));
        wunit.commit();
    }

    const std::string indexName = "a_1";
    const BSONObj spec =
        BSON("ns" << collNs.ns() << "v" << 2 << "name" << indexName << "key" << BSON("a" << 1)
                  << "unique"
                  << true
                  << "background"
                  << true);

    // Create the index build block. Insert two different documents, but each with the same value
    // for 'a'. This will cause the index insert to allow inserting a duplicate key.
    const IndexCatalogEntry* entry;

    boost::optional<Record> record1;
    boost::optional<Record> record2;
    {
        AutoGetCollection autoColl(opCtx(), collNs, MODE_X);
        auto coll = autoColl.getCollection();

        MultiIndexBlock indexer(opCtx(), coll);
        // Don't use the bulk builder, which does not insert directly into the IAM for the index.
        indexer.allowBackgroundBuilding();
        // Allow duplicates.
        indexer.ignoreUniqueConstraint();

        ASSERT_OK(indexer.init(spec).getStatus());

        IndexDescriptor* desc = coll->getIndexCatalog()->findIndexByName(
            opCtx(), indexName, true /* includeUnfinished */);
        ASSERT(desc);
        entry = coll->getIndexCatalog()->getEntry(desc);

        // Construct the tracker.
        tracker = makeTracker(opCtx(), entry);

        // Index the documents.
        WriteUnitOfWork wunit(opCtx());
        auto cursor = coll->getCursor(opCtx());

        record1 = cursor->next();
        ASSERT(record1);

        std::vector<BSONObj> dupsInserted;

        // The insert of the first document should return no duplicates.
        ASSERT_OK(indexer.insert(record1->data.releaseToBson(), record1->id, &dupsInserted));
        ASSERT_EQ(0u, dupsInserted.size());

        // The insert of the second document should return that a duplicate key was inserted.
        record2 = cursor->next();
        ASSERT(record2);
        ASSERT_OK(indexer.insert(record2->data.releaseToBson(), record2->id, &dupsInserted));
        ASSERT_EQ(1u, dupsInserted.size());

        // Record that duplicates were inserted.
        AutoGetCollection tempColl(opCtx(), tracker->nss(), MODE_IX);
        ASSERT_OK(tracker->recordDuplicates(opCtx(), tempColl.getCollection(), dupsInserted));

        ASSERT_OK(indexer.commit());
        wunit.commit();
    }

    // Confirm that the keys + RecordId of the duplicate are recorded.
    {
        AutoGetCollection tempColl(opCtx(), tracker->nss(), MODE_IS);
        Status s = tracker->constraintsSatisfiedForIndex(opCtx(), tempColl.getCollection());
        ASSERT_EQ(ErrorCodes::DuplicateKey, s.code());
    }

    // Now remove the document and index key to confirm the conflicts are resolved.
    {
        AutoGetCollection autoColl(opCtx(), collNs, MODE_IX);
        auto coll = autoColl.getCollection();

        WriteUnitOfWork wunit(opCtx());

        OpDebug opDebug;
        coll->deleteDocument(opCtx(), kUninitializedStmtId, record2->id, &opDebug);
        wunit.commit();

        // One key deleted for each index (includes _id)
        ASSERT_EQ(2u, *opDebug.additiveMetrics.keysDeleted);

        AutoGetCollection tempColl(opCtx(), tracker->nss(), MODE_IS);
        Status s = tracker->constraintsSatisfiedForIndex(opCtx(), tempColl.getCollection());
        ASSERT_OK(s);
    }

    destroyTempCollection(opCtx(), tracker->nss());
}

TEST_F(DuplicateKeyTrackerTest, BulkIndexBuild) {
    std::unique_ptr<DuplicateKeyTracker> tracker;

    const NamespaceString collNs("test.myCollection");
    const BSONObj doc1 = BSON("_id" << 1 << "a" << 1);
    const BSONObj doc2 = BSON("_id" << 2 << "a" << 1);

    // Create the collection with a two documents that have the same key for 'a'.
    {
        AutoGetOrCreateDb dbRaii(opCtx(), collNs.db(), LockMode::MODE_X);
        WriteUnitOfWork wunit(opCtx());
        CollectionOptions options;
        options.uuid = UUID::gen();
        auto coll = dbRaii.getDb()->createCollection(opCtx(), collNs.ns(), options);
        ASSERT(coll);

        ASSERT_OK(coll->insertDocument(opCtx(), InsertStatement(doc1), nullptr));
        ASSERT_OK(coll->insertDocument(opCtx(), InsertStatement(doc2), nullptr));
        wunit.commit();
    }

    const std::string indexName = "a_1";
    const BSONObj spec =
        BSON("ns" << collNs.ns() << "v" << 2 << "name" << indexName << "key" << BSON("a" << 1)
                  << "unique"
                  << true
                  << "background"
                  << true);

    // Create the bulk build block. Insert two different documents, but each with the same value
    // for 'a'. This will cause the index insert to allow inserting a duplicate key.
    const IndexCatalogEntry* entry;

    boost::optional<Record> record1;
    boost::optional<Record> record2;
    {
        AutoGetCollection autoColl(opCtx(), collNs, MODE_X);
        auto coll = autoColl.getCollection();

        MultiIndexBlock indexer(opCtx(), coll);
        // Allow duplicates.
        indexer.ignoreUniqueConstraint();

        ASSERT_OK(indexer.init(spec).getStatus());

        IndexDescriptor* desc = coll->getIndexCatalog()->findIndexByName(
            opCtx(), indexName, true /* includeUnfinished */);
        entry = coll->getIndexCatalog()->getEntry(desc);

        // Construct the tracker.
        tracker = makeTracker(opCtx(), entry);

        auto cursor = coll->getCursor(opCtx());

        record1 = cursor->next();
        ASSERT(record1);

        std::vector<BSONObj> dupsInserted;

        // Neither of these inserts will recognize duplicates because the bulk inserter does not
        // detect them until dumpInsertsFromBulk() is called.
        ASSERT_OK(indexer.insert(record1->data.releaseToBson(), record1->id, &dupsInserted));
        ASSERT_EQ(0u, dupsInserted.size());

        record2 = cursor->next();
        ASSERT(record2);
        ASSERT_OK(indexer.insert(record2->data.releaseToBson(), record2->id, &dupsInserted));
        ASSERT_EQ(0u, dupsInserted.size());

        ASSERT_OK(indexer.dumpInsertsFromBulk(&dupsInserted));
        ASSERT_EQ(1u, dupsInserted.size());

        // Record that duplicates were inserted.
        WriteUnitOfWork wunit(opCtx());

        AutoGetCollection tempColl(opCtx(), tracker->nss(), MODE_IX);
        ASSERT_OK(tracker->recordDuplicates(opCtx(), tempColl.getCollection(), dupsInserted));

        ASSERT_OK(indexer.commit());
        wunit.commit();
    }

    // Confirm that the keys + RecordId of the duplicate are recorded.
    {
        AutoGetCollection tempColl(opCtx(), tracker->nss(), MODE_IS);
        Status s = tracker->constraintsSatisfiedForIndex(opCtx(), tempColl.getCollection());
        ASSERT_EQ(ErrorCodes::DuplicateKey, s.code());
    }

    // Now remove the document and index key to confirm the conflicts are resolved.
    {
        AutoGetCollection autoColl(opCtx(), collNs, MODE_IX);
        auto coll = autoColl.getCollection();

        WriteUnitOfWork wunit(opCtx());

        OpDebug opDebug;
        coll->deleteDocument(opCtx(), kUninitializedStmtId, record2->id, &opDebug);
        wunit.commit();

        // One key deleted for each index (includes _id)
        ASSERT_EQ(2u, *opDebug.additiveMetrics.keysDeleted);

        AutoGetCollection tempColl(opCtx(), tracker->nss(), MODE_IS);
        Status s = tracker->constraintsSatisfiedForIndex(opCtx(), tempColl.getCollection());
        ASSERT_OK(s);
    }

    destroyTempCollection(opCtx(), tracker->nss());
}
}  // namespace
}  // namespace mongo

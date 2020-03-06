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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/validate_state.h"

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace {

const NamespaceString kNss = NamespaceString("fooDB.fooColl");

class ValidateStateTest : public CatalogTestFixture {
public:
    ValidateStateTest() : CatalogTestFixture("wiredTiger") {}

    /**
     * Create collection 'nss' and insert some documents. It will possess a default _id index.
     */
    Collection* createCollectionAndPopulateIt(OperationContext* opCtx, const NamespaceString& nss);
};

Collection* ValidateStateTest::createCollectionAndPopulateIt(OperationContext* opCtx,
                                                             const NamespaceString& nss) {
    // Create collection.
    CollectionOptions defaultCollectionOptions;
    ASSERT_OK(storageInterface()->createCollection(opCtx, nss, defaultCollectionOptions));

    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    Collection* collection = autoColl.getCollection();
    invariant(collection);

    // Insert some data.
    OpDebug* const nullOpDebug = nullptr;
    for (int i = 0; i < 10; i++) {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(
            collection->insertDocument(opCtx, InsertStatement(BSON("_id" << i)), nullOpDebug));
        wuow.commit();
    }

    return collection;
}

/**
 * Builds an index on the given 'nss'. 'indexKey' specifies the index key, e.g. {'a': 1};
 */
void createIndex(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& indexKey) {
    DBDirectClient client(opCtx);
    client.createIndex(nss.ns(), indexKey);
}

/**
 * Drops index 'indexName' in collection 'nss'.
 */
void dropIndex(OperationContext* opCtx, const NamespaceString& nss, const std::string& indexName) {
    AutoGetCollection autoColl(opCtx, nss, MODE_X);

    WriteUnitOfWork wuow(opCtx);

    auto collection = autoColl.getCollection();
    auto indexDescriptor = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    ASSERT(indexDescriptor);
    ASSERT_OK(collection->getIndexCatalog()->dropIndex(opCtx, indexDescriptor));

    wuow.commit();
}

// ValidateState constructor should throw if the collection doesn't exist.
TEST_F(ValidateStateTest, NonExistentCollectionShouldThrowNamespaceNotFoundError) {
    auto opCtx = operationContext();

    ASSERT_THROWS_CODE(CollectionValidation::ValidateState(
                           opCtx,
                           kNss,
                           /*background*/ false,
                           CollectionValidation::ValidateOptions::kNoFullValidation),
                       AssertionException,
                       ErrorCodes::NamespaceNotFound);

    ASSERT_THROWS_CODE(CollectionValidation::ValidateState(
                           opCtx,
                           kNss,
                           /*background*/ true,
                           CollectionValidation::ValidateOptions::kNoFullValidation),
                       AssertionException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(ValidateStateTest, UncheckpointedCollectionShouldBeAbleToInitializeCursors) {
    auto opCtx = operationContext();

    // Disable periodic checkpoint'ing thread so we can control when checkpoints occur.
    FailPointEnableBlock failPoint("pauseCheckpointThread");

    // Checkpoint of all of the data.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ false);

    createCollectionAndPopulateIt(opCtx, kNss);
    CollectionValidation::ValidateState validateState(
        opCtx, kNss, /*background*/ true, CollectionValidation::ValidateOptions::kNoFullValidation);
    // Assert that cursors are able to created on the new collection.
    validateState.initializeCursors(opCtx);
    // There should only be a first record id if cursors were initialized successfully.
    ASSERT(!validateState.getFirstRecordId().isNull());
}

// Basic test with {background:false} to open cursors against all collection indexes.
TEST_F(ValidateStateTest, OpenCursorsOnAllIndexes) {
    // Disable index build commit quorum as we don't have support of replication subsystem for
    // voting.
    ASSERT_OK(ServerParameterSet::getGlobal()
                  ->getMap()
                  .find("enableIndexBuildCommitQuorum")
                  ->second->setFromString("false"));
    auto opCtx = operationContext();
    createCollectionAndPopulateIt(opCtx, kNss);

    // Disable periodic checkpoint'ing thread so we can control when checkpoints occur.
    FailPointEnableBlock failPoint("pauseCheckpointThread");

    // Create several indexes.
    createIndex(opCtx, kNss, BSON("a" << 1));
    createIndex(opCtx, kNss, BSON("b" << 1));
    createIndex(opCtx, kNss, BSON("c" << 1));
    createIndex(opCtx, kNss, BSON("d" << 1));

    {
        // Open the cursors.
        CollectionValidation::ValidateState validateState(
            opCtx,
            kNss,
            /*background*/ false,
            CollectionValidation::ValidateOptions::kNoFullValidation);
        validateState.initializeCursors(opCtx);

        // Make sure all of the indexes were found and cursors opened against them. Including the
        // _id index.
        ASSERT_EQ(validateState.getIndexes().size(), 5);
    }

    // Checkpoint of all of the data: it should not make any difference for foreground validation
    // that does not use checkpoint cursors.
    // Note: no locks can be held for a waitUntilDurable*() call.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ false);

    // Check that foreground validation behaves just the same with checkpoint'ed data.
    CollectionValidation::ValidateState validateState(
        opCtx,
        kNss,
        /*background*/ false,
        CollectionValidation::ValidateOptions::kNoFullValidation);
    validateState.initializeCursors(opCtx);
    ASSERT_EQ(validateState.getIndexes().size(), 5);
}

// Open cursors against all indexes with {background:true}.
TEST_F(ValidateStateTest, OpenCursorsOnAllIndexesWithBackground) {
    // Disable index build commit quorum as we don't have support of replication subsystem for
    // voting.
    ASSERT_OK(ServerParameterSet::getGlobal()
                  ->getMap()
                  .find("enableIndexBuildCommitQuorum")
                  ->second->setFromString("false"));

    auto opCtx = operationContext();
    createCollectionAndPopulateIt(opCtx, kNss);

    // Disable periodic checkpoint'ing thread so we can control when checkpoints occur.
    FailPointEnableBlock failPoint("pauseCheckpointThread");

    // Create two indexes and checkpoint them.
    createIndex(opCtx, kNss, BSON("a" << 1));
    createIndex(opCtx, kNss, BSON("b" << 1));
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ false);

    // Create two more indexes that are not checkpoint'ed.
    createIndex(opCtx, kNss, BSON("c" << 1));
    createIndex(opCtx, kNss, BSON("d" << 1));

    // Open the cursors.
    CollectionValidation::ValidateState validateState(
        opCtx, kNss, /*background*/ true, CollectionValidation::ValidateOptions::kNoFullValidation);
    validateState.initializeCursors(opCtx);

    // We should be able to open a cursor on each index.
    // (Note the _id index was create with collection creation, so we have 5 indexes.)
    ASSERT_EQ(validateState.getIndexes().size(), 5);
}

// Indexes in the checkpoint that were dropped in the present should not have cursors opened against
// them.
TEST_F(ValidateStateTest, CursorsAreNotOpenedAgainstCheckpointedIndexesThatWereLaterDropped) {
    // Disable index build commit quorum as we don't have support of replication subsystem for
    // voting.
    ASSERT_OK(ServerParameterSet::getGlobal()
                  ->getMap()
                  .find("enableIndexBuildCommitQuorum")
                  ->second->setFromString("false"));

    auto opCtx = operationContext();
    createCollectionAndPopulateIt(opCtx, kNss);

    // Disable periodic checkpoint'ing thread so we can control when checkpoints occur.
    FailPointEnableBlock failPoint("pauseCheckpointThread");

    // Create several indexes.
    createIndex(opCtx, kNss, BSON("a" << 1));
    createIndex(opCtx, kNss, BSON("b" << 1));
    createIndex(opCtx, kNss, BSON("c" << 1));
    createIndex(opCtx, kNss, BSON("d" << 1));

    // Checkpoint the indexes.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ false);

    // Drop two indexes without checkpoint'ing the drops.
    dropIndex(opCtx, kNss, "a_1");
    dropIndex(opCtx, kNss, "b_1");

    // Open cursors and check that the two dropped indexes are not found.
    // (Note the _id index was create with collection creation, so we have 3 indexes.)
    {
        CollectionValidation::ValidateState validateState(
            opCtx,
            kNss,
            /*background*/ true,
            CollectionValidation::ValidateOptions::kNoFullValidation);
        validateState.initializeCursors(opCtx);
        ASSERT_EQ(validateState.getIndexes().size(), 3);
    }

    // Checkpoint the index drops and recheck that the indexes are not found.
    // Note: no locks can be held for a waitUntilDurable*() call.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ false);

    CollectionValidation::ValidateState validateState(
        opCtx, kNss, /*background*/ true, CollectionValidation::ValidateOptions::kNoFullValidation);
    validateState.initializeCursors(opCtx);
    ASSERT_EQ(validateState.getIndexes().size(), 3);
}

}  // namespace
}  // namespace mongo

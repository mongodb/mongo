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
#include "mongo/util/log.h"

namespace mongo {

namespace {

const NamespaceString kNss = NamespaceString("fooDB.fooColl");

class ValidateStateTest : public CatalogTestFixture {
public:
    ValidateStateTest() : CatalogTestFixture("wiredTiger") {}

    /**
     * Create collection 'nss' and insert some documents. It will possess a default _id index.
     */
    void createCollectionAndPopulateIt(OperationContext* opCtx, const NamespaceString& nss);
};

void ValidateStateTest::createCollectionAndPopulateIt(OperationContext* opCtx,
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
                           opCtx, kNss, /*background*/ false, /*fullValidate*/ false),
                       AssertionException,
                       ErrorCodes::NamespaceNotFound);

    ASSERT_THROWS_CODE(CollectionValidation::ValidateState(
                           opCtx, kNss, /*background*/ true, /*fullValidate*/ false),
                       AssertionException,
                       ErrorCodes::NamespaceNotFound);
}

// Validate with {background:true} should fail to find an uncheckpoint'ed collection.
TEST_F(ValidateStateTest, UncheckpointedCollectionShouldThrowCursorNotFoundError) {
    auto opCtx = operationContext();

    // Disable periodic checkpoint'ing thread so we can control when checkpoints occur.
    FailPointEnableBlock failPoint("pauseCheckpointThread");

    // Make sure there is a checkpoint.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx);  // provokes a checkpoint.

    // Create the collection, which will not be in the checkpoint, and check that a CursorNotFound
    // error is thrown when attempting to open cursors.
    createCollectionAndPopulateIt(opCtx, kNss);
    CollectionValidation::ValidateState validateState(
        opCtx, kNss, /*background*/ true, /*fullValidate*/ false);
    ASSERT_THROWS_CODE(
        validateState.initializeCursors(opCtx), AssertionException, ErrorCodes::CursorNotFound);
}

// Basic test with {background:false} to open cursors against all collection indexes.
TEST_F(ValidateStateTest, OpenCursorsOnAllIndexes) {
    auto opCtx = operationContext();
    createCollectionAndPopulateIt(opCtx, kNss);

    // Disable periodic checkpoint'ing thread so we can control when checkpoints occur.
    FailPointEnableBlock failPoint("pauseCheckpointThread");

    // Create several indexes.
    createIndex(opCtx, kNss, BSON("a" << 1));
    createIndex(opCtx, kNss, BSON("b" << 1));
    createIndex(opCtx, kNss, BSON("c" << 1));
    createIndex(opCtx, kNss, BSON("d" << 1));

    // Open the cursors.
    CollectionValidation::ValidateState validateState(
        opCtx, kNss, /*background*/ false, /*fullValidate*/ false);
    validateState.initializeCursors(opCtx);

    // Make sure all of the indexes were found and cursors opened against them. Including the _id
    // index.
    ASSERT_EQ(validateState.getIndexes().size(), 5);

    // Force a checkpoint: it should not make any difference for foreground validation that does not
    // use checkpoint cursors.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx);  // provokes a checkpoint.

    // Check that foreground validation behaves just the same with checkpoint'ed data.
    CollectionValidation::ValidateState validateState2(
        opCtx, kNss, /*background*/ false, /*fullValidate*/ false);
    validateState2.initializeCursors(opCtx);
    ASSERT_EQ(validateState2.getIndexes().size(), 5);
}

// Open cursors against checkpoint'ed indexes with {background:true}.
TEST_F(ValidateStateTest, OpenCursorsOnCheckpointedIndexes) {
    auto opCtx = operationContext();
    createCollectionAndPopulateIt(opCtx, kNss);

    // Disable periodic checkpoint'ing thread so we can control when checkpoints occur.
    FailPointEnableBlock failPoint("pauseCheckpointThread");

    // Create two indexes and checkpoint them.
    createIndex(opCtx, kNss, BSON("a" << 1));
    createIndex(opCtx, kNss, BSON("b" << 1));
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx);  // provokes a checkpoint.

    // Create two more indexes that are not checkpoint'ed.
    createIndex(opCtx, kNss, BSON("c" << 1));
    createIndex(opCtx, kNss, BSON("d" << 1));

    // Open the cursors.
    CollectionValidation::ValidateState validateState(
        opCtx, kNss, /*background*/ true, /*fullValidate*/ false);
    validateState.initializeCursors(opCtx);

    // Make sure the uncheckpoint'ed indexes are not found.
    // (Note the _id index was create with collection creation, so we have 3 indexes.)
    ASSERT_EQ(validateState.getIndexes().size(), 3);
}

// Only open cursors against indexes that are consistent with the rest of the checkpoint'ed data.
TEST_F(ValidateStateTest, OpenCursorsOnConsistentlyCheckpointedIndexes) {
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
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx);  // provokes a checkpoint.

    {
        // Artificially set two indexes as inconsistent with the checkpoint.
        AutoGetCollection autoColl(opCtx, kNss, MODE_IS);
        auto checkpointLock =
            opCtx->getServiceContext()->getStorageEngine()->getCheckpointLock(opCtx);
        auto indexIdentA =
            opCtx->getServiceContext()->getStorageEngine()->getCatalog()->getIndexIdent(
                opCtx, kNss, "a_1");
        auto indexIdentB =
            opCtx->getServiceContext()->getStorageEngine()->getCatalog()->getIndexIdent(
                opCtx, kNss, "b_1");
        opCtx->getServiceContext()->getStorageEngine()->addIndividuallyCheckpointedIndexToList(
            indexIdentA);
        opCtx->getServiceContext()->getStorageEngine()->addIndividuallyCheckpointedIndexToList(
            indexIdentB);
    }

    // The two inconsistent indexes should not be found.
    // (Note the _id index was create with collection creation, so we have 3 indexes.)
    CollectionValidation::ValidateState validateState(
        opCtx, kNss, /*background*/ true, /*fullValidate*/ false);
    validateState.initializeCursors(opCtx);
    ASSERT_EQ(validateState.getIndexes().size(), 3);
}

// Indexes in the checkpoint that were dropped in the present should not have cursors opened against
// them.
TEST_F(ValidateStateTest, CursorsAreNotOpenedAgainstCheckpointedIndexesThatWereLaterDropped) {
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
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx);  // provokes a checkpoint.

    // Drop two indexes without checkpoint'ing the drops.
    dropIndex(opCtx, kNss, "a_1");
    dropIndex(opCtx, kNss, "b_1");

    // Open cursors and check that the two dropped indexes are not found.
    // (Note the _id index was create with collection creation, so we have 3 indexes.)
    CollectionValidation::ValidateState validateState(
        opCtx, kNss, /*background*/ true, /*fullValidate*/ false);
    validateState.initializeCursors(opCtx);
    ASSERT_EQ(validateState.getIndexes().size(), 3);

    // Checkpoint the index drops and recheck that the indexes are not found.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx);  // provokes a checkpoint.
    CollectionValidation::ValidateState validateState2(
        opCtx, kNss, /*background*/ true, /*fullValidate*/ false);
    validateState2.initializeCursors(opCtx);
    ASSERT_EQ(validateState2.getIndexes().size(), 3);
}

}  // namespace
}  // namespace mongo

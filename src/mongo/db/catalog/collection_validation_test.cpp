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

#include "mongo/bson/util/builder.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString("test.t");

class CollectionValidationTest : public CatalogTestFixture {
protected:
    CollectionValidationTest(Options options = {}) : CatalogTestFixture(std::move(options)) {}

private:
    void setUp() override {
        CatalogTestFixture::setUp();

        // Create collection kNss for unit tests to use. It will possess a default _id index.
        CollectionOptions defaultCollectionOptions;
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), kNss, defaultCollectionOptions));
    };
};

// Background validation opens checkpoint cursors which requires reading from the disk.
class CollectionValidationDiskTest : public CollectionValidationTest {
protected:
    CollectionValidationDiskTest() : CollectionValidationTest(Options{}.ephemeral(false)) {}
};

/**
 * Calls validate on collection kNss with both kValidateFull and kValidateNormal validation levels
 * and verifies the results.
 *
 * Returns list of validate results.
 */
std::vector<std::pair<BSONObj, ValidateResults>> foregroundValidate(
    OperationContext* opCtx,
    bool valid,
    int numRecords,
    int numInvalidDocuments,
    int numErrors,
    std::initializer_list<CollectionValidation::ValidateMode> modes =
        {CollectionValidation::ValidateMode::kForeground,
         CollectionValidation::ValidateMode::kForegroundFull},
    CollectionValidation::RepairMode repairMode = CollectionValidation::RepairMode::kNone) {
    std::vector<std::pair<BSONObj, ValidateResults>> results;
    for (auto mode : modes) {
        ValidateResults validateResults;
        BSONObjBuilder output;
        ASSERT_OK(CollectionValidation::validate(
            opCtx, kNss, mode, repairMode, &validateResults, &output));
        BSONObj obj = output.obj();
        BSONObjBuilder validateResultsBuilder;
        validateResults.appendToResultObj(&validateResultsBuilder, true /* debugging */);
        auto validateResultsObj = validateResultsBuilder.obj();

        ASSERT_EQ(validateResults.valid, valid) << obj << validateResultsObj;
        ASSERT_EQ(validateResults.errors.size(), static_cast<long unsigned int>(numErrors))
            << obj << validateResultsObj;

        ASSERT_EQ(obj.getIntField("nrecords"), numRecords) << obj << validateResultsObj;
        ASSERT_EQ(obj.getIntField("nInvalidDocuments"), numInvalidDocuments)
            << obj << validateResultsObj;

        results.push_back(std::make_pair(obj, validateResults));
    }
    return results;
}

ValidateResults omitTransientWarnings(const ValidateResults& results) {
    ValidateResults copy = results;
    copy.warnings.clear();
    for (const auto& warning : results.warnings) {
        std::string endMsg =
            "This is a transient issue as the collection was actively in use by other "
            "operations.";
        std::string beginMsg = "Could not complete validation of ";
        if (warning.size() >= std::max(endMsg.size(), beginMsg.size())) {
            bool startsWith = std::equal(beginMsg.begin(), beginMsg.end(), warning.begin());
            bool endsWith = std::equal(endMsg.rbegin(), endMsg.rend(), warning.rbegin());
            if (!(startsWith && endsWith)) {
                copy.warnings.emplace_back(warning);
            }
        } else {
            copy.warnings.emplace_back(warning);
        }
    }
    return copy;
}

/**
 * Calls validate on collection kNss with {background:true} and verifies the results.
 * If 'runForegroundAsWell' is set, then foregroundValidate() above will be run in addition.
 */
void backgroundValidate(OperationContext* opCtx,
                        bool valid,
                        int numRecords,
                        int numInvalidDocuments,
                        int numErrors,
                        bool runForegroundAsWell) {
    if (runForegroundAsWell) {
        foregroundValidate(opCtx, valid, numRecords, numInvalidDocuments, numErrors);
    }

    // This function will force a checkpoint, so background validation can then read from that
    // checkpoint.
    // Set 'stableCheckpoint' to false, so we checkpoint ALL data, not just up to WT's
    // stable_timestamp.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableTimestamp*/ false);

    ValidateResults validateResults;
    BSONObjBuilder output;
    ASSERT_OK(CollectionValidation::validate(opCtx,
                                             kNss,
                                             CollectionValidation::ValidateMode::kBackground,
                                             CollectionValidation::RepairMode::kNone,
                                             &validateResults,
                                             &output));
    BSONObj obj = output.obj();

    ASSERT_EQ(validateResults.valid, valid);
    ASSERT_EQ(validateResults.errors.size(), static_cast<long unsigned int>(numErrors));

    ASSERT_EQ(obj.getIntField("nrecords"), numRecords);
    ASSERT_EQ(obj.getIntField("nInvalidDocuments"), numInvalidDocuments);
}

/**
 * Inserts a range of documents into the kNss collection and then returns that count.
 * The range is defined by [startIDNum, endIDNum), not inclusive of endIDNum, using the numbers as
 * values for '_id' of the document being inserted.
 */
int insertDataRange(OperationContext* opCtx, int startIDNum, int endIDNum) {
    invariant(startIDNum < endIDNum,
              str::stream() << "attempted to insert invalid data range from " << startIDNum
                            << " to " << endIDNum);


    AutoGetCollection coll(opCtx, kNss, MODE_IX);
    std::vector<InsertStatement> inserts;
    for (int i = startIDNum; i < endIDNum; ++i) {
        auto doc = BSON("_id" << i);
        inserts.push_back(InsertStatement(doc));
    }

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(collection_internal::insertDocuments(
            opCtx, *coll, inserts.begin(), inserts.end(), nullptr, false));
        wuow.commit();
    }
    return endIDNum - startIDNum;
}

/**
 * Inserts a single invalid document into the kNss collection and then returns that count.
 */
int setUpInvalidData(OperationContext* opCtx) {
    AutoGetCollection coll(opCtx, kNss, MODE_IX);
    RecordStore* rs = coll->getRecordStore();

    {
        WriteUnitOfWork wuow(opCtx);
        auto invalidBson = "\0\0\0\0\0"_sd;
        ASSERT_OK(
            rs->insertRecord(opCtx, invalidBson.rawData(), invalidBson.size(), Timestamp::min())
                .getStatus());
        wuow.commit();
    }

    return 1;
}

// Verify that calling validate() on an empty collection with different validation levels returns an
// OK status.
TEST_F(CollectionValidationTest, ValidateEmpty) {
    foregroundValidate(operationContext(),
                       /*valid*/ true,
                       /*numRecords*/ 0,
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0);
}
TEST_F(CollectionValidationDiskTest, BackgroundValidateEmpty) {
    backgroundValidate(operationContext(),
                       /*valid*/ true,
                       /*numRecords*/ 0,
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0,
                       /*runForegroundAsWell*/ true);
}

// Verify calling validate() on a nonempty collection with different validation levels.
TEST_F(CollectionValidationTest, Validate) {
    auto opCtx = operationContext();
    foregroundValidate(opCtx,
                       /*valid*/ true,
                       /*numRecords*/ insertDataRange(opCtx, 0, 5),
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0);
}
TEST_F(CollectionValidationDiskTest, BackgroundValidate) {
    auto opCtx = operationContext();
    backgroundValidate(opCtx,
                       /*valid*/ true,
                       /*numRecords*/ insertDataRange(opCtx, 0, 5),
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0,
                       /*runForegroundAsWell*/ true);
}

// Verify calling validate() on a collection with an invalid document.
TEST_F(CollectionValidationTest, ValidateError) {
    auto opCtx = operationContext();
    foregroundValidate(opCtx,
                       /*valid*/ false,
                       /*numRecords*/ setUpInvalidData(opCtx),
                       /*numInvalidDocuments*/ 1,
                       /*numErrors*/ 1);
}
TEST_F(CollectionValidationDiskTest, BackgroundValidateError) {
    auto opCtx = operationContext();
    backgroundValidate(opCtx,
                       /*valid*/ false,
                       /*numRecords*/ setUpInvalidData(opCtx),
                       /*numInvalidDocuments*/ 1,
                       /*numErrors*/ 1,
                       /*runForegroundAsWell*/ true);
}

// Verify calling validate() with enforceFastCount=true.
TEST_F(CollectionValidationTest, ValidateEnforceFastCount) {
    auto opCtx = operationContext();
    foregroundValidate(opCtx,
                       /*valid*/ true,
                       /*numRecords*/ insertDataRange(opCtx, 0, 5),
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0,
                       {CollectionValidation::ValidateMode::kForegroundFullEnforceFastCount});
}

/**
 * Waits for a parallel running collection validation operation to start and then hang at a
 * failpoint.
 *
 * A failpoint in the validate() code should have been set prior to calling this function.
 */
void waitUntilValidateFailpointHasBeenReached() {
    while (!CollectionValidation::getIsValidationPausedForTest()) {
        sleepmillis(100);  // a fairly arbitrary sleep period.
    }
    ASSERT(CollectionValidation::getIsValidationPausedForTest());
}

TEST_F(CollectionValidationDiskTest, BackgroundValidateRunsConcurrentlyWithWrites) {
    auto opCtx = operationContext();
    auto serviceContext = opCtx->getServiceContext();

    // Set up some data in the collection so that we can validate it.
    int numRecords = insertDataRange(opCtx, 0, 5);

    stdx::thread runBackgroundValidate;
    int numRecords2;
    {
        // Set a failpoint in the collection validation code and then start a parallel operation to
        // run background validation in parallel.
        FailPointEnableBlock failPoint("pauseCollectionValidationWithLock");
        runBackgroundValidate = stdx::thread([&serviceContext, &numRecords] {
            ThreadClient tc("BackgroundValidateConcurrentWithCRUD-thread", serviceContext);
            auto threadOpCtx = tc->makeOperationContext();
            backgroundValidate(
                threadOpCtx.get(), true, numRecords, 0, 0, /*runForegroundAsWell*/ false);
        });

        // Wait until validate starts and hangs mid-way on a failpoint, then do concurrent writes,
        // which should succeed and not affect the background validation.
        waitUntilValidateFailpointHasBeenReached();
        numRecords2 = insertDataRange(opCtx, 5, 15);
    }

    // Make sure the background validation finishes successfully.
    runBackgroundValidate.join();

    // Run regular foreground collection validation to make sure everything is OK.
    foregroundValidate(opCtx,
                       /*valid*/ true,
                       /*numRecords*/ numRecords + numRecords2,
                       0,
                       0);
}

/**
 * Generates a KeyString suitable for positioning a cursor at the beginning of an index.
 */
KeyString::Value makeFirstKeyString(const SortedDataInterface& sortedDataInterface) {
    KeyString::Builder firstKeyStringBuilder(sortedDataInterface.getKeyStringVersion(),
                                             BSONObj(),
                                             sortedDataInterface.getOrdering(),
                                             KeyString::Discriminator::kExclusiveBefore);
    return firstKeyStringBuilder.getValueCopy();
}

/**
 * Extracts KeyString without RecordId.
 */
KeyString::Value makeKeyStringWithoutRecordId(const KeyString::Value& keyStringWithRecordId,
                                              KeyString::Version version) {
    BufBuilder bufBuilder;
    keyStringWithRecordId.serializeWithoutRecordIdLong(bufBuilder);
    auto builderSize = bufBuilder.len();

    auto buffer = bufBuilder.release();

    BufReader bufReader(buffer.get(), builderSize);
    return KeyString::Value::deserialize(bufReader, version);
}

// Verify calling validate() on a collection with old (pre-4.2) keys in a WT unique index.
TEST_F(CollectionValidationTest, ValidateOldUniqueIndexKeyWarning) {
    auto opCtx = operationContext();

    {
        // Durable catalog expects metadata updates to be timestamped but this is
        // not necessary in our case - we just want to check the contents of the index table.
        // The alternative here would be to provide a commit timestamp with a TimestamptBlock.
        repl::UnreplicatedWritesBlock uwb(opCtx);
        auto uniqueIndexSpec = BSON("v" << 2 << "name"
                                        << "a_1"
                                        << "key" << BSON("a" << 1) << "unique" << true);
        ASSERT_OK(
            storageInterface()->createIndexesOnEmptyCollection(opCtx, kNss, {uniqueIndexSpec}));
    }

    // Insert single document with the default (new) index key that includes a record id.
    ASSERT_OK(storageInterface()->insertDocument(opCtx,
                                                 kNss,
                                                 {BSON("_id" << 1 << "a" << 1), Timestamp()},
                                                 repl::OpTime::kUninitializedTerm));

    // Validate the collection here as a sanity check before we modify the index contents in-place.
    foregroundValidate(
        opCtx, /*valid*/ true, /*numRecords*/ 1, /*numInvalidDocuments*/ 0, /*numErrors*/ 0);

    // Update existing entry in index to pre-4.2 format without record id in key string.
    {
        AutoGetCollection autoColl(opCtx, kNss, MODE_IX);

        auto indexCatalog = autoColl->getIndexCatalog();
        auto descriptor = indexCatalog->findIndexByName(opCtx, "a_1");
        ASSERT(descriptor) << "Cannot find a_1 in index catalog";
        auto entry = indexCatalog->getEntry(descriptor);
        ASSERT(entry) << "Cannot look up index catalog entry for index a_1";

        auto sortedDataInterface = entry->accessMethod()->asSortedData()->getSortedDataInterface();
        ASSERT_FALSE(sortedDataInterface->isEmpty(opCtx)) << "index a_1 should not be empty";

        // Check key in index for only document.
        auto firstKeyString = makeFirstKeyString(*sortedDataInterface);
        KeyString::Value keyStringWithRecordId;
        RecordId recordId;
        {
            auto cursor = sortedDataInterface->newCursor(opCtx);
            auto indexEntry = cursor->seekForKeyString(firstKeyString);
            ASSERT(indexEntry);
            ASSERT(cursor->isRecordIdAtEndOfKeyString());
            keyStringWithRecordId = indexEntry->keyString;
            recordId = indexEntry->loc;
            ASSERT_FALSE(cursor->nextKeyString());
        }

        auto keyStringWithoutRecordId = makeKeyStringWithoutRecordId(
            keyStringWithRecordId, sortedDataInterface->getKeyStringVersion());

        // Replace key with old format (without record id).
        {
            WriteUnitOfWork wuow(opCtx);
            bool dupsAllowed = false;
            sortedDataInterface->unindex(opCtx, keyStringWithRecordId, dupsAllowed);
            sortedDataInterface->insertWithRecordIdInValue_forTest(
                opCtx, keyStringWithoutRecordId, recordId);
            wuow.commit();
        }

        // Confirm that key in index is in old format.
        {
            auto cursor = sortedDataInterface->newCursor(opCtx);
            auto indexEntry = cursor->seekForKeyString(firstKeyString);
            ASSERT(indexEntry);
            ASSERT_FALSE(cursor->isRecordIdAtEndOfKeyString());
            ASSERT_EQ(indexEntry->keyString.compareWithoutRecordIdLong(keyStringWithRecordId), 0);
            ASSERT_FALSE(cursor->nextKeyString());
        }
    }

    auto results = foregroundValidate(opCtx,
                                      /*valid*/ true,
                                      /*numRecords*/ 1,
                                      /*numInvalidDocuments*/ 0,
                                      /*numErrors*/ 0);
    ASSERT_EQ(results.size(), 2);

    for (const auto& result : results) {
        const auto& validateResults = result.second;
        BSONObjBuilder builder;
        bool debugging = true;
        validateResults.appendToResultObj(&builder, debugging);
        auto obj = builder.obj();
        ASSERT(validateResults.valid) << obj;
        auto warningsWithoutTransientErrors = omitTransientWarnings(validateResults);
        ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 1U) << obj;
        ASSERT_STRING_CONTAINS(warningsWithoutTransientErrors.warnings[0],
                               "Unique index a_1 has one or more keys in the old format")
            << obj;
    }
}

}  // namespace
}  // namespace mongo

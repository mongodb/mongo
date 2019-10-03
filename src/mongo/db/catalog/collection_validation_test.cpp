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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_validation.h"

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace {

const NamespaceString kNss = NamespaceString("test.t");

/**
 * Test fixture for collection validation with the ephemeralForTest storage engine.
 * Validation with {background:true} is not supported by the ephemeralForTest storage engine.
 */
class CollectionValidationTest : public CatalogTestFixture {
public:
    CollectionValidationTest() : CollectionValidationTest("ephemeralForTest") {}

protected:
    /**
     * Allow inheriting classes to select a storage engine with which to run unit tests.
     */
    explicit CollectionValidationTest(std::string engine) : CatalogTestFixture(std::move(engine)) {}

private:
    void setUp() override {
        CatalogTestFixture::setUp();

        // Create collection kNss for unit tests to use. It will possess a default _id index.
        CollectionOptions defaultCollectionOptions;
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), kNss, defaultCollectionOptions));
    };
};

/**
 * Test fixture for testing background collection validation on the wiredTiger engine, which is
 * currently the only storage engine that supports background collection validation.
 *
 * Collection kNss will be created for each unit test, curtesy of inheritance from
 * CollectionValidationTest.
 */
class BackgroundCollectionValidationTest : public CollectionValidationTest {
public:
    /**
     * Sets up the wiredTiger storage engine that supports data checkpointing.
     *
     * Background validation runs on a checkpoint, and therefore only on storage engines that
     * support checkpoints.
     */
    BackgroundCollectionValidationTest() : CollectionValidationTest("wiredTiger") {}
};

/**
 * Calls validate on collection kNss with both kValidateFull and kValidateNormal validation levels
 * and verifies the results.
 */
void foregroundValidate(
    OperationContext* opCtx, bool valid, int numRecords, int numInvalidDocuments, int numErrors) {
    std::vector<bool> levels = {false, true};
    for (auto level : levels) {
        ValidateResults validateResults;
        BSONObjBuilder output;
        ASSERT_OK(CollectionValidation::validate(
            opCtx, kNss, level, /*background*/ false, &validateResults, &output));
        ASSERT_EQ(validateResults.valid, valid);
        ASSERT_EQ(validateResults.errors.size(), static_cast<long unsigned int>(numErrors));

        BSONObj obj = output.obj();
        ASSERT_EQ(obj.getIntField("nrecords"), numRecords);
        ASSERT_EQ(obj.getIntField("nInvalidDocuments"), numInvalidDocuments);
    }
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
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx);

    ValidateResults validateResults;
    BSONObjBuilder output;
    ASSERT_OK(CollectionValidation::validate(opCtx,
                                             kNss,
                                             /*fullValidate*/ false,
                                             /*background*/ true,
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


    AutoGetCollection autoColl(opCtx, kNss, MODE_IX);
    Collection* coll = autoColl.getCollection();
    std::vector<InsertStatement> inserts;
    for (int i = startIDNum; i < endIDNum; ++i) {
        auto doc = BSON("_id" << i);
        inserts.push_back(InsertStatement(doc));
    }

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(coll->insertDocuments(opCtx, inserts.begin(), inserts.end(), nullptr, false));
        wuow.commit();
    }
    return endIDNum - startIDNum;
}

/**
 * Inserts a single invalid document into the kNss collection and then returns that count.
 */
int setUpInvalidData(OperationContext* opCtx) {
    AutoGetCollection autoColl(opCtx, kNss, MODE_IX);
    Collection* coll = autoColl.getCollection();
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
    // Running on the ephemeralForTest storage engine.
    foregroundValidate(operationContext(),
                       /*valid*/ true,
                       /*numRecords*/ 0,
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0);
}
TEST_F(BackgroundCollectionValidationTest, BackgroundValidateEmpty) {
    // Running on the WT storage engine.
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
TEST_F(BackgroundCollectionValidationTest, BackgroundValidate) {
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
TEST_F(BackgroundCollectionValidationTest, BackgroundValidateError) {
    auto opCtx = operationContext();
    backgroundValidate(opCtx,
                       /*valid*/ false,
                       /*numRecords*/ setUpInvalidData(opCtx),
                       /*numInvalidDocuments*/ 1,
                       /*numErrors*/ 1,
                       /*runForegroundAsWell*/ true);
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

TEST_F(BackgroundCollectionValidationTest, BackgroundValidateRunsConcurrentlyWithWrites) {
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
    foregroundValidate(opCtx, /*valid*/ true, /*numRecords*/ numRecords + numRecords2, 0, 0);
}

}  // namespace
}  // namespace mongo

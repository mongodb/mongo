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
#include "mongo/unittest/unittest.h"

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
    std::vector<ValidateCmdLevel> levels{kValidateNormal, kValidateFull};
    for (auto level : levels) {
        ValidateResults validateResults;
        BSONObjBuilder output;
        ASSERT_OK(
            CollectionValidation::validate(opCtx, kNss, level, false, &validateResults, &output));
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

    ValidateResults validateResults;
    BSONObjBuilder output;
    ASSERT_OK(CollectionValidation::validate(opCtx,
                                             kNss,
                                             ValidateCmdLevel::kValidateNormal,
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
 * Inserts a few documents into the kNss collection and then returns that count.
 */
int setUpValidData(OperationContext* opCtx) {
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
    Collection* coll = autoColl.getCollection();

    std::vector<InsertStatement> inserts;
    int numRecords = 5;
    for (int i = 0; i < numRecords; i++) {
        auto doc = BSON("_id" << i);
        inserts.push_back(InsertStatement(doc));
    }

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(coll->insertDocuments(opCtx, inserts.begin(), inserts.end(), nullptr, false));
        wuow.commit();
    }
    return numRecords;
}

/**
 * Inserts a single invalid document into the kNss collection and then returns that count.
 */
int setUpInvalidData(OperationContext* opCtx) {
    AutoGetCollection autoColl(opCtx, kNss, MODE_X);
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

// TODO (SERVER-42223): right now background validation is mostly identical to foreground, except
// that background runs with an IX lock instead of a X lock. SERVER-42223 will set up real
// background validation.

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
                       /*numRecords*/ setUpValidData(opCtx),
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0);
}
TEST_F(BackgroundCollectionValidationTest, BackgroundValidate) {
    auto opCtx = operationContext();
    backgroundValidate(opCtx,
                       /*valid*/ true,
                       /*numRecords*/ setUpValidData(opCtx),
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

}  // namespace
}  // namespace mongo

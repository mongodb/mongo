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

class CollectionValidationTest : public CatalogTestFixture {
private:
    void setUp() override;

public:
    void checkValidate(Collection* coll, bool valid, int records, int invalid, int errors);

    std::vector<ValidateCmdLevel> levels{kValidateNormal, kValidateFull};
};

void CollectionValidationTest::setUp() {
    CatalogTestFixture::setUp();
    CollectionOptions defaultCollectionOptions;
    ASSERT_OK(
        storageInterface()->createCollection(operationContext(), kNss, defaultCollectionOptions));
}

// Call validate with different validation levels and verify the results.
void CollectionValidationTest::checkValidate(
    Collection* coll, bool valid, int records, int invalid, int errors) {
    auto opCtx = operationContext();
    auto collLock = std::make_unique<Lock::CollectionLock>(opCtx, coll->ns(), MODE_X);

    for (auto level : levels) {
        ValidateResults results;
        BSONObjBuilder output;
        auto status = CollectionValidation::validate(opCtx, coll, level, false, &results, &output);
        ASSERT_OK(status);
        ASSERT_EQ(results.valid, valid);
        ASSERT_EQ(results.errors.size(), (long unsigned int)errors);

        BSONObj obj = output.obj();
        ASSERT_EQ(obj.getIntField("nrecords"), records);
        ASSERT_EQ(obj.getIntField("nInvalidDocuments"), invalid);
    }
}

// Verify that calling validate() on an empty collection with different validation levels returns an
// OK status.
TEST_F(CollectionValidationTest, ValidateEmpty) {
    auto opCtx = operationContext();
    AutoGetCollection agc(opCtx, kNss, MODE_X);
    Collection* coll = agc.getCollection();

    checkValidate(coll, true, 0, 0, 0);
}

// Verify calling validate() on a nonempty collection with different validation levels.
TEST_F(CollectionValidationTest, Validate) {
    auto opCtx = operationContext();
    AutoGetCollection agc(opCtx, kNss, MODE_X);
    Collection* coll = agc.getCollection();

    std::vector<InsertStatement> inserts;
    for (int i = 0; i < 5; i++) {
        auto doc = BSON("_id" << i);
        inserts.push_back(InsertStatement(doc));
    }

    auto status = coll->insertDocuments(opCtx, inserts.begin(), inserts.end(), nullptr, false);
    ASSERT_OK(status);
    checkValidate(coll, true, inserts.size(), 0, 0);
}

// Verify calling validate() on a collection with an invalid document.
TEST_F(CollectionValidationTest, ValidateError) {
    auto opCtx = operationContext();
    AutoGetCollection agc(opCtx, kNss, MODE_X);
    Collection* coll = agc.getCollection();
    RecordStore* rs = coll->getRecordStore();

    auto invalidBson = "\0\0\0\0\0"_sd;
    auto statusWithId =
        rs->insertRecord(opCtx, invalidBson.rawData(), invalidBson.size(), Timestamp::min());
    ASSERT_OK(statusWithId.getStatus());
    checkValidate(coll, false, 1, 1, 1);
}

}  // namespace

}  // namespace mongo

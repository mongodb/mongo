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

#include "mongo/db/validate/collection_validation.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/db/timeseries/viewless_timeseries_collection_creation_helpers.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.t");

class CollectionValidationTest : public CatalogTestFixture {
protected:
    CollectionValidationTest(Options options = {}) : CatalogTestFixture(std::move(options)) {}

private:
    void setUp() override {
        CatalogTestFixture::setUp();

        // Create collection kNss for unit tests to use. It will possess a default _id index.
        const CollectionOptions defaultCollectionOptions;
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), kNss, defaultCollectionOptions));
    };
};

// Calling verify() is not possible on an in-memory instance.
class CollectionValidationDiskTest : public CollectionValidationTest {
protected:
    CollectionValidationDiskTest() : CollectionValidationTest(Options{}.inMemory(false)) {}
};

struct ForegroundValidateTestResults {
    bool valid{true};
    int numRecords{0};
    int numInvalidDocuments{0};
    int numNonCompliantDocuments{0};
    int numErrors{0};
    int numWarnings{0};
    auto operator<=>(const ForegroundValidateTestResults&) const = default;
    friend std::ostream& operator<<(std::ostream& os, const ForegroundValidateTestResults& results);
};

inline std::string stringify_forTest(const ForegroundValidateTestResults& fgRes) {
    return fmt::format(
        "{{valid={}, numRecords={}, numInvalidDocuments={}, numErrors={}, numWarnings={}}}",
        fgRes.valid,
        fgRes.numRecords,
        fgRes.numInvalidDocuments,
        fgRes.numErrors,
        fgRes.numWarnings);
}
std::ostream& operator<<(std::ostream& os, const ForegroundValidateTestResults& results) {
    os << stringify_forTest(results);
    return os;
}

/**
 * Calls validate on collection nss with both kValidateFull and kValidateNormal validation levels
 * and verifies the results.
 *
 * Returns the list of validation results.
 */
std::vector<ValidateResults> foregroundValidate(
    const NamespaceString& nss,
    OperationContext* opCtx,
    const ForegroundValidateTestResults& expected,
    std::initializer_list<CollectionValidation::ValidateMode> modes =
        {CollectionValidation::ValidateMode::kForeground,
         CollectionValidation::ValidateMode::kForegroundFull,
         CollectionValidation::ValidateMode::kForegroundFullCheckBSON},
    CollectionValidation::RepairMode repairMode = CollectionValidation::RepairMode::kNone) {

    std::vector<ValidateResults> results;

    for (const auto mode : modes) {
        ValidateResults validateResults;
        EXPECT_EQ(ErrorCodes::OK,
                  CollectionValidation::validate(
                      opCtx,
                      nss,
                      CollectionValidation::ValidationOptions{mode,
                                                              repairMode,
                                                              /*logDiagnostics=*/false},
                      &validateResults))
            << "Validation Mode: " << static_cast<int>(mode);
        BSONObjBuilder validateResultsBuilder;
        validateResults.appendToResultObj(&validateResultsBuilder, true /* debugging */);
        auto validateResultsObj = validateResultsBuilder.obj();

        // The total number of errors is: those in the top-level results plus the sum of
        // all index-specific errors.
        const int observedNumErrors = validateResults.getErrors().size() +
            std::accumulate(validateResults.getIndexResultsMap().begin(),
                            validateResults.getIndexResultsMap().end(),
                            0,
                            [](size_t current, const auto& ivr) {
                                return current + ivr.second.getErrors().size();
                            });

        const ForegroundValidateTestResults actual{
            .valid = validateResults.isValid(),
            .numRecords = validateResultsObj.getIntField("nrecords"),
            .numInvalidDocuments = validateResultsObj.getIntField("nInvalidDocuments"),
            .numErrors = observedNumErrors,
            .numWarnings = static_cast<int>(validateResults.getWarnings().size())};

        EXPECT_EQ(expected, actual) << validateResultsObj;
        results.push_back(std::move(validateResults));
    }
    return results;
}

/**
 * Inserts a range of documents into the nss collection and then returns that count. The range is
 * defined by [startIDNum, startIDNum+numDocs), not inclusive of (startIDNum+numDocs), using the
 * numbers as values for '_id' of the document being inserted followed by numFields fields.
 */
int insertDataRangeForNumFields(const NamespaceString& nss,
                                OperationContext* opCtx,
                                const int startIDNum,
                                const int numDocs,
                                const int numFields) {
    const AutoGetCollection coll(opCtx, nss, MODE_IX);
    std::vector<BSONObj> inserts;
    for (int i = 0; i < numDocs; ++i) {
        BSONObjBuilder bsonBuilder;
        bsonBuilder << "_id" << i + startIDNum;
        for (int c = 1; c <= numFields; ++c) {
            bsonBuilder << "a" + std::to_string(c) << i + (i * numFields + startIDNum) + c;
        }
        const auto obj = bsonBuilder.obj();
        inserts.push_back(obj);
    }

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, *coll, inserts));
        wuow.commit();
    }
    return numDocs;
}

/**
 * Inserts a range of documents into the kNss collection and then returns that count. The range is
 * defined by [startIDNum, endIDNum), not inclusive of endIDNum, using the numbers as values for
 * '_id' of the document being inserted.
 */
int insertDataRange(OperationContext* opCtx, const int startIDNum, const int endIDNum) {
    invariant(startIDNum < endIDNum,
              str::stream() << "attempted to insert invalid data range from " << startIDNum
                            << " to " << endIDNum);

    return insertDataRangeForNumFields(kNss, opCtx, startIDNum, endIDNum - startIDNum, 0);
}

int setUpDataOfGivenSize(OperationContext* opCtx,
                         int targetBsonSize,
                         const NamespaceString& nss = kNss) {
    ASSERT_TRUE(opCtx);
    AutoGetCollection coll(opCtx, nss, MODE_IX);

    // Build a temporary BSON object to determine the overhead. This overhead will need to be
    // subtracted from the test objects to set their sizes close to limits.
    const int overhead = std::invoke([] {
        BSONObjBuilder builder;
        std::string testStr("test");
        builder.append("_id", testStr);
        return builder.obj().objsize() - testStr.size();
    });

    BSONObjBuilder builder;
    builder.append("_id", std::string(targetBsonSize - overhead, 'a'));
    BSONObj oversizeObj = builder.obj();
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, *coll, oversizeObj))
            << "Failed to insert object of size " << targetBsonSize;
        wuow.commit();
    }
    return 1;
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
        ASSERT_OK(rs->insertRecord(opCtx,
                                   *shard_role_details::getRecoveryUnit(opCtx),
                                   invalidBson.data(),
                                   invalidBson.size(),
                                   Timestamp::min())
                      .getStatus());
        wuow.commit();
    }

    return 1;
}

/**
 * Convenience function to convert ValidateResults to a BSON object.
 */
BSONObj resultToBSON(const ValidateResults& vr) {
    BSONObjBuilder builder;
    vr.appendToResultObj(&builder, true /* debugging */);
    return builder.obj();
}


// Verify that calling validate() on an empty collection with different validation levels returns an
// OK status.
TEST_F(CollectionValidationTest, ValidateEmpty) {
    foregroundValidate(kNss,
                       operationContext(),
                       {.valid = true,
                        .numRecords = 0,
                        .numInvalidDocuments = 0,
                        .numErrors = 0,
                        .numWarnings = 0});
}

// Verify calling validate() on a nonempty collection with different validation levels.
TEST_F(CollectionValidationTest, Validate) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = true,
                        .numRecords = insertDataRange(opCtx, 0, 5),
                        .numInvalidDocuments = 0,
                        .numErrors = 0});
}

// Verify calling validate() on a collection with an invalid document.
TEST_F(CollectionValidationTest, ValidateError) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = false,
                        .numRecords = setUpInvalidData(opCtx),
                        .numInvalidDocuments = 1,
                        .numErrors = 1});
}

// Verify calling validate() with enforceFastCount=true.
TEST_F(CollectionValidationTest, ValidateEnforceFastCount) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = true,
                        .numRecords = insertDataRange(opCtx, 0, 5),
                        .numInvalidDocuments = 0,
                        .numErrors = 0},
                       {CollectionValidation::ValidateMode::kForegroundFullEnforceFastCount});
}

TEST_F(CollectionValidationTest, ValidateCollectionDocumentSizeUserLimit) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = true,
                        .numRecords = setUpDataOfGivenSize(opCtx, BSONObjMaxUserSize),
                        .numInvalidDocuments = 0,
                        .numErrors = 0,
                        .numWarnings = 0},
                       {CollectionValidation::ValidateMode::kForegroundCheckBSON});
}

TEST_F(CollectionValidationTest, ValidateCollectionDocumentSizeOverUserLimit) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = false,
                        .numRecords = setUpDataOfGivenSize(opCtx, BSONObjMaxUserSize + 1),
                        .numInvalidDocuments = 1,
                        .numErrors = 1,
                        .numWarnings = 0},
                       {CollectionValidation::ValidateMode::kForegroundCheckBSON});
}

TEST_F(CollectionValidationTest, ValidateCollectionDocumentSizeInternalLimit) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = false,
                        .numRecords = setUpDataOfGivenSize(opCtx, BSONObjMaxInternalSize),
                        .numInvalidDocuments = 1,
                        .numErrors = 1,
                        .numWarnings = 0},
                       {CollectionValidation::ValidateMode::kForegroundCheckBSON});
}

TEST_F(CollectionValidationTest, ValidateCollectionDocumentSizeOverInternalLimit) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = false,
                        .numRecords = setUpDataOfGivenSize(opCtx, BSONObjMaxInternalSize + 1),
                        .numInvalidDocuments = 1,
                        .numErrors = 1,
                        .numWarnings = 0},
                       {CollectionValidation::ValidateMode::kForegroundCheckBSON});
}

TEST_F(CollectionValidationTest, ValidateCollectionDocumentMixedSizes) {
    auto opCtx = operationContext();
    setUpDataOfGivenSize(opCtx, BSONObjMaxInternalSize);
    setUpDataOfGivenSize(opCtx, BSONObjMaxInternalSize + 1);
    setUpDataOfGivenSize(opCtx, BSONObjMaxUserSize);
    setUpDataOfGivenSize(opCtx, BSONObjMaxUserSize + 1);
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = false,
                        .numRecords = 4,
                        .numInvalidDocuments = 3,
                        .numErrors = 1,
                        .numWarnings = 0},
                       {CollectionValidation::ValidateMode::kForegroundCheckBSON});
}

TEST_F(CollectionValidationDiskTest, ValidateIndexDetailResultsSurfaceVerifyErrors) {
    FailPointEnableBlock fp{"WTValidateIndexStructuralDamage"};
    auto opCtx = operationContext();
    insertDataRange(opCtx, 0, 5);  // initialize collection
    foregroundValidate(
        kNss,
        opCtx,
        {.valid = false,
         .numRecords = std::numeric_limits<int32_t>::min(),           // uninitialized
         .numInvalidDocuments = std::numeric_limits<int32_t>::min(),  // uninitialized
         .numErrors = 1,
         .numWarnings = 1},
        {CollectionValidation::ValidateMode::kForegroundFull});
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

/**
 * Generates a KeyString suitable for positioning a cursor at the beginning of an index.
 */
key_string::Value makeFirstKeyString(const SortedDataInterface& sortedDataInterface) {
    key_string::Builder firstKeyStringBuilder(sortedDataInterface.getKeyStringVersion(),
                                              BSONObj(),
                                              sortedDataInterface.getOrdering(),
                                              key_string::Discriminator::kExclusiveBefore);
    return firstKeyStringBuilder.getValueCopy();
}

/**
 * Extracts KeyString without RecordId.
 */
key_string::Value makeKeyStringWithoutRecordId(const key_string::View& keyStringWithRecordId,
                                               key_string::Version version) {
    BufBuilder bufBuilder;
    keyStringWithRecordId.serializeWithoutRecordId(bufBuilder);
    auto builderSize = bufBuilder.len();

    auto buffer = bufBuilder.release();

    BufReader bufReader(buffer.get(), builderSize);
    return key_string::Value::deserialize(bufReader, version, boost::none /* ridFormat */);
}

// Verify calling validate() on a collection with old (pre-4.2) keys in a WT unique index.
TEST_F(CollectionValidationTest, ValidateOldUniqueIndexKeyWarning) {
    auto opCtx = operationContext();

    {
        FailPointEnableBlock createOldFormatIndex("WTIndexCreateUniqueIndexesInOldFormat");

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
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = true,
                        .numRecords = 1,
                        .numInvalidDocuments = 0,
                        .numErrors = 0,
                        .numWarnings = 0});

    // Update existing entry in index to pre-4.2 format without record id in key string.
    {
        AutoGetCollection autoColl(opCtx, kNss, MODE_IX);

        auto indexCatalog = autoColl->getIndexCatalog();
        auto entry = indexCatalog->findIndexByName(opCtx, "a_1");
        ASSERT(entry) << "Cannot find a_1 in index catalog";

        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

        auto sortedDataInterface = entry->accessMethod()->asSortedData()->getSortedDataInterface();
        ASSERT_FALSE(sortedDataInterface->isEmpty(opCtx, ru)) << "index a_1 should not be empty";

        // Check key in index for only document.
        auto first = makeFirstKeyString(*sortedDataInterface);
        auto firstKeyString = first.getView();
        key_string::Value keyStringWithRecordId;
        {
            auto cursor = sortedDataInterface->newCursor(opCtx, ru);
            auto indexEntry = cursor->seekForKeyString(ru, firstKeyString);
            ASSERT(indexEntry);
            ASSERT(cursor->isRecordIdAtEndOfKeyString());
            keyStringWithRecordId = indexEntry->keyString;
            ASSERT_FALSE(cursor->nextKeyString(ru));
        }

        // Replace key with old format (without record id).
        {
            WriteUnitOfWork wuow(opCtx);
            bool dupsAllowed = false;
            sortedDataInterface->unindex(opCtx, ru, keyStringWithRecordId, dupsAllowed);
            FailPointEnableBlock insertOldFormatKeys("WTIndexInsertUniqueKeysInOldFormat");
            ASSERT_SDI_INSERT_OK(
                sortedDataInterface->insert(opCtx, ru, keyStringWithRecordId, dupsAllowed));
            wuow.commit();
        }

        // Confirm that key in index is in old format.
        {
            auto cursor = sortedDataInterface->newCursor(opCtx, ru);
            auto indexEntry = cursor->seekForKeyString(ru, firstKeyString);
            ASSERT(indexEntry);
            ASSERT_FALSE(cursor->isRecordIdAtEndOfKeyString());
            ASSERT_EQ(indexEntry->keyString.compareWithoutRecordIdLong(keyStringWithRecordId), 0);
            ASSERT_FALSE(cursor->nextKeyString(ru));
        }
    }

    const auto results = foregroundValidate(kNss,
                                            opCtx,
                                            {.valid = true,
                                             .numRecords = 1,
                                             .numInvalidDocuments = 0,
                                             .numErrors = 0,
                                             .numWarnings = 1});
    EXPECT_EQ(results.size(), 3);

    for (const auto& validateResults : results) {
        const auto obj = resultToBSON(validateResults);
        ASSERT(validateResults.isValid()) << obj;
        auto isOldFormat = [](const auto& warn) {
            return warn.find("Unique index a_1 has one or more keys in the old format") !=
                std::string::npos;
        };
        ASSERT(std::any_of(validateResults.getWarnings().begin(),
                           validateResults.getWarnings().end(),
                           isOldFormat))
            << obj;
    }
}

TEST_F(CollectionValidationTest, HashPrefixesEmptyString) {
    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({""}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({""}, /*equalLength=*/false),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesTooLong) {
    constexpr int kHashStringMaxLen = 64;
    ASSERT_DOES_NOT_THROW(CollectionValidation::validateHashes(
        {std::string(kHashStringMaxLen, 'A')}, /*equalLength=*/true));

    ASSERT_THROWS_CODE(CollectionValidation::validateHashes(
                           {std::string(kHashStringMaxLen + 1, 'A')}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(CollectionValidation::validateHashes(
                           {std::string(kHashStringMaxLen + 1, 'A')}, /*equalLength=*/false),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesDifferentLengths) {
    ASSERT_DOES_NOT_THROW(
        CollectionValidation::validateHashes({"AAA", "BBBB"}, /*equalLength=*/false));

    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({"AAA", "BBBB"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesHexString) {
    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({"NOTHEX"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({"NOTHEX"}, /*equalLength=*/false),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesDuplicates) {
    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({"ABC", "ABC"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(
        CollectionValidation::validateHashes({"ABC", "ABCD", "A"}, /*equalLength=*/false),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesCases) {
    constexpr int kHashStringMaxLen = 64;
    ASSERT_DOES_NOT_THROW(
        CollectionValidation::validateHashes({"AAA1", "BBB1", "CCC1"}, /*equalLength=*/true));
    ASSERT_DOES_NOT_THROW(CollectionValidation::validateHashes(
        {std::string(kHashStringMaxLen, 'A')}, /*equalLength=*/true));

    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({"a"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({"AAA", "BBBB"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({"nothex"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(CollectionValidation::validateHashes({"AAA", "AAA"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(
        CollectionValidation::validateHashes({"abcd", "a", "ABCDEF"}, /*equalLength=*/true),
        DBException,
        ErrorCodes::InvalidOptions);
}

enum class SchemaViolationTestMode { ExpectFailOnDocumentInsert, ExpectFailOnCollectionValidation };
std::ostream& operator<<(std::ostream& os, SchemaViolationTestMode mode) {
    switch (mode) {
        case SchemaViolationTestMode::ExpectFailOnDocumentInsert:
            os << "ExpectFailOnDocumentInsert";
            break;
        case SchemaViolationTestMode::ExpectFailOnCollectionValidation:
            os << "ExpectFailOnCollectionValidation";
            break;
    }
    return os;
}

using CollectionValidationSchemaViolationParams = std::tuple<BSONObj, SchemaViolationTestMode>;
class CollectionValidationSchemaViolationTest
    : public CatalogTestFixture,
      public ::testing::WithParamInterface<CollectionValidationSchemaViolationParams> {
protected:
    CollectionValidationSchemaViolationTest(Options options = {})
        : CatalogTestFixture(std::move(options)) {}

private:
    OperationContext* _opCtx{nullptr};
    void setUp() override {
        CatalogTestFixture::setUp();
    }
};

TEST_P(CollectionValidationSchemaViolationTest, SchemaViolation) {
    // Schema violation in non-timeseries emits a warning
    auto opCtx = operationContext();
    const auto [doc, mode] = GetParam();
    const auto validatorDoc = fromjson(R"BSON(
        { $jsonSchema:
          {
            bsonType: "object",
            required: ["requiredField"],
            properties: {
              requiredField: { bsonType: "int", minimum: 1, maximum: 100 }
            }
          }
        }
        )BSON");
    const auto validationAction = mode == SchemaViolationTestMode::ExpectFailOnDocumentInsert
        ? ValidationActionEnum::errorAndLog
        : ValidationActionEnum::warn;

    {
        ASSERT_OK(storageInterface()->createCollection(
            opCtx, kNss, {.validator = validatorDoc, .validationAction = validationAction}));
        WriteUnitOfWork wuow(opCtx);
        const AutoGetCollection coll(opCtx, kNss, MODE_IX);
        ASSERT_FALSE(coll->isTimeseriesCollection());

        switch (mode) {
            case SchemaViolationTestMode::ExpectFailOnDocumentInsert:
                ASSERT_EQ(Helpers::insert(opCtx, *coll, doc).code(),
                          ErrorCodes::DocumentValidationFailure);
                return;
            case SchemaViolationTestMode::ExpectFailOnCollectionValidation:
                ASSERT_OK(Helpers::insert(opCtx, *coll, doc));
                wuow.commit();
                break;
        }
    }
    foregroundValidate(kNss, opCtx, {.valid = true, .numRecords = 1, .numWarnings = 1});
}

INSTANTIATE_TEST_SUITE_P(
    SchemaViolations,
    CollectionValidationSchemaViolationTest,
    testing::Combine(testing::Values(BSON("_id" << "x" << "requiredField" << "y"),
                                     BSON("_id" << "x"),
                                     BSON("_id" << "x" << "requiredField" << 142)),
                     testing::Values(SchemaViolationTestMode::ExpectFailOnCollectionValidation,
                                     SchemaViolationTestMode::ExpectFailOnDocumentInsert)));

template <class T = BSONObj>
BSONObj replaceNestedField(const BSONObj& bson,
                           std::span<const StringData> nestedFieldNames,
                           const T& replacement,
                           boost::optional<StringData> replacementFieldName = boost::none) {
    invariant(!nestedFieldNames.empty());
    const StringData cur = nestedFieldNames.front();
    BSONObjBuilder bob;
    // Ordering must be preserved to avoid out of order errors, so fields must be added
    // one-by-one.
    for (const auto& elem : bson) {
        if (elem.fieldNameStringData() == cur) {
            if (nestedFieldNames.size() == 1) {
                // Allow for changing the field name to inject schema naming errors.
                if constexpr (std::is_same_v<T, BSONElement>) {
                    bob.appendAs(replacement, replacementFieldName.value_or(cur));
                } else {
                    bob.append(replacementFieldName.value_or(cur), replacement);
                }
            } else {
                bob.append(
                    cur,
                    replaceNestedField(elem.Obj(),
                                       {nestedFieldNames.begin() + 1, nestedFieldNames.end()},
                                       replacement,
                                       replacementFieldName));
            }
        } else {
            bob.append(elem);
        }
    }
    return bob.obj();
}

BSONObj removeNestedField(const BSONObj& bson, std::span<const StringData> nestedFieldNames) {
    invariant(!nestedFieldNames.empty());
    const StringData cur = nestedFieldNames.front();
    auto removed = bson.removeField(cur);
    if (nestedFieldNames.size() == 1) {
        return removed;
    } else if (bson.hasField(cur)) {
        BSONObjBuilder bob;
        bob.appendElements(removed);
        bob.append(cur,
                   removeNestedField(bson.getField(cur).Obj(),
                                     {nestedFieldNames.begin() + 1, nestedFieldNames.end()}));
        return bob.obj();
    }
    MONGO_UNREACHABLE;
}

class TimeseriesCollectionValidationTest : public CatalogTestFixture {
public:
    TimeseriesCollectionValidationTest(Options options = {})
        : CatalogTestFixture(std::move(options)) {
        _nss = NamespaceString::createNamespaceString_forTest("test.system.buckets.ts");
    }

    static BSONObj getSampleDoc() {
        return ::mongo::fromjson(R"BSON(
        {
            "_id" : {"$oid": "61be04541ad72e8d5d257550"},
            "control" : {
                "version" : 2,
                "min" : {
                    "_id" : {"$oid": "6912089d8302f7b0ec8eef41"},
                    "date" : {"$date": "2021-12-18T15:55:00Z"},
                    "close" : 252.47,
                    "volume" : 27890
                },
                "max" : {
                    "_id" : {"$oid": "6912089d8302f7b0ec8eef45"},
                    "date" : {"$date": "2021-12-18T15:59:00Z"},
                    "close" : 254.03,
                    "volume" : 55046
                },
                "count" : 5
            },
            "meta": {"ticker" : "MDB"}, 
            "data" : {
                "_id" : {"$binary": "BwBpEgidgwL3sOyO70WAGwAAAAAAAAAA", "$type":"07"},
                "date" : {"$binary": "CQAg6EDOfQEAAIEMTB0AAAAAAA4AAAAAAAAAAA==", "$type":"07"},
                "close" : {"$binary": "AQApXI/C9cBvQLD7BBgAHAK2AAA=", "$type":"07"},
                "volume" : {"$binary": "AQAAAAAAwKnjQJB7C0YAo3jwqwA=", "$type":"07"}
            }
        })BSON");
    }

    static BSONObj getVersion3ControlSampleDoc() {
        return ::mongo::fromjson(R"BSON(
        {
            "_id": {"$oid": "69690e4c8bee049fcc1c4d87"},
            "control": {
                "version": 3,
                "min": {
                    "date": {"$date": "2026-01-15T15:57:00Z"},
                    "data": "a",
                    "_id": {"$oid": "69690e642a688d1103284d0c"}
                },
                "max": {
                    "date": {"$date": "2026-01-15T15:57:32.636Z"},
                    "data": "d",
                    "_id": {"$oid": "69690ea22a688d1103284d10"}
                },
                "count": 5
            },
            "data": {
                "data": {"$binary": {"base64": "AgACAAAAYQCBLAAAAgAgAADOPwwAAAAAAAA=", "subType": "07"}},
                "date": {"$binary": {"base64": "CQDpOGDCmwEAAICLLuFtJFWCTwA=", "subType": "07"}},
                "_id": {"$binary": {"base64": "BwBpaQ5kKmiNEQMoTQyAK2AAEPwXANQA", "subType": "07"}}
            }
        })BSON");
    }

    static BSONObj getExtendedTimeRangeSampleDoc() {
        return ::mongo::fromjson(R"BSON(
        {
            "_id": {"$oid":"e980f6cc8bee049fcc1c4d88"},
            "control": {
                "version": 2,
                "min": {
                    "date": {"$date": {"$numberLong": "-377424180000"}},
                    "data": "bb",
                    "_id": {"$oid":"6969340c2a688d1103284d11"}
                },
                "max": {
                    "date": {"$date": {"$numberLong": "-377424151000"}},
                    "data": "bb",
                    "_id": {"$oid":"6969340c2a688d1103284d11"}
                },
                "count": 1
            },
            "data": {
                "data": {"$binary": {"base64": "AgADAAAAYmIAAA==", "subType": "07"}},
                "date": {"$binary": {"base64": "CQAofsQfqP///wA=", "subType": "07"}},
                "_id": {"$binary": {"base64": "BwBpaTQMKmiNEQMoTREA", "subType": "07"}}
            }
        })BSON");
    }

    static std::vector<BSONObj> getAllSampleDocs() {
        return {getSampleDoc(), getVersion3ControlSampleDoc(), getExtendedTimeRangeSampleDoc()};
    };

    static constexpr auto replacementIncorrectTimeField = "t"_sd;

    void insertDoc(BSONObj doc, ErrorCodes::Error expected = ErrorCodes::OK) {
        ASSERT_OK(storageInterface()->createCollection(_opCtx, _nss, _options));
        WriteUnitOfWork wuow(_opCtx);
        AutoGetCollection coll(_opCtx, _nss, MODE_IX);
        ASSERT_TRUE(coll->isTimeseriesCollection());
        EXPECT_EQ(expected, Helpers::insert(_opCtx, *coll, doc));
        wuow.commit();
    }

    static BSONObj getSampleDocMismatchedMeasurementField(StringData measurementField) {
        const auto origBson = getSampleDoc();
        const BSONColumn colData(origBson.getObjectField("data"_sd).firstElement());

        BSONColumnBuilder bcb;
        const size_t sz = colData.size();
        // Copy one less element to mangle the data.
        for (size_t copied = 0; const auto& datum : colData) {
            if (copied == sz - 1) {
                break;
            }
            bcb.append(datum);
            ++copied;
        }

        const std::vector<StringData> nested = {"data"_sd, measurementField};
        return replaceNestedField(origBson, nested, bcb.finalize());
    }

    NamespaceString _nss;
    OperationContext* _opCtx{nullptr};
    CollectionOptions _options;
    CollectionValidation::ValidateMode _validateMode{
        CollectionValidation::ValidateMode::kForeground};

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _options.uuid = UUID::gen();
        _options.timeseries = TimeseriesOptions(/*timeField*/ "date");
        _options.timeseries->setBucketRoundingSeconds(60);
        _options.timeseries->setBucketMaxSpanSeconds(60 * 60 * 24);
        _options.timeseries->setMetaField("ticker"_sd);
        _options.timeseries->setGranularity(BucketGranularityEnum::Seconds);
        _options.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
        _options.validationAction = ValidationActionEnum::errorAndLog;
        _options.validator = timeseries::generateTimeseriesValidator(
            timeseries::kTimeseriesControlCompressedSortedVersion,
            _options.timeseries->getTimeField());
        _opCtx = operationContext();
    };
};

class TimeseriesCollectionValidationValidBucketsTest : public TimeseriesCollectionValidationTest,
                                                       public testing::WithParamInterface<BSONObj> {
};
TEST_P(TimeseriesCollectionValidationValidBucketsTest, TimeseriesValidationGoodData) {
    const BSONObj bson = GetParam();
    ASSERT_OK(storageInterface()->createCollection(_opCtx, _nss, _options));
    {
        WriteUnitOfWork wuow(_opCtx);
        AutoGetCollection coll(_opCtx, _nss, MODE_IX);
        coll->setRequiresTimeseriesExtendedRangeSupport(_opCtx);
        ASSERT_TRUE(coll->isTimeseriesCollection());
        ASSERT_OK(Helpers::insert(_opCtx, *coll, bson));
        wuow.commit();
    }
    foregroundValidate(
        _nss, _opCtx, {.valid = true, .numRecords = 1, .numErrors = 0, .numWarnings = 0});
}
INSTANTIATE_TEST_SUITE_P(ValidBuckets,
                         TimeseriesCollectionValidationValidBucketsTest,
                         testing::ValuesIn(TimeseriesCollectionValidationTest::getAllSampleDocs()));

class TimeseriesCollectionValidationSchemaViolationTest
    : public TimeseriesCollectionValidationTest,
      public testing::WithParamInterface<SchemaViolationTestMode> {
protected:
    void setUp() override {
        TimeseriesCollectionValidationTest::setUp();
        switch (GetParam()) {
            case SchemaViolationTestMode::ExpectFailOnDocumentInsert:
                // no-op
                break;
            case SchemaViolationTestMode::ExpectFailOnCollectionValidation:
                _options.validationAction = ValidationActionEnum::warn;
                break;
        }
    }  // namespace
};  // namespace mongo

INSTANTIATE_TEST_SUITE_P(SchemaViolations,
                         TimeseriesCollectionValidationSchemaViolationTest,
                         testing::Values(SchemaViolationTestMode::ExpectFailOnDocumentInsert,
                                         SchemaViolationTestMode::ExpectFailOnCollectionValidation),
                         [](const ::testing::TestParamInfo<SchemaViolationTestMode>& info)
                             -> std::string {
                             switch (info.param) {
                                 case SchemaViolationTestMode::ExpectFailOnDocumentInsert:
                                     return "OnDocumentInsert";
                                 case SchemaViolationTestMode::ExpectFailOnCollectionValidation:
                                     return "OnCollectionValidation";
                             }
                             MONGO_UNREACHABLE;
                         });

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationBadBucketSpan) {
    _options.timeseries->setBucketMaxSpanSeconds(30);
    insertDoc(getSampleDoc());
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationBadControlCount) {
    static constexpr std::array nested = {"control"_sd, "count"_sd};
    insertDoc(replaceNestedField(getSampleDoc(), nested, 4));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_P(TimeseriesCollectionValidationSchemaViolationTest, TimeseriesValidationMissingMin) {
    static constexpr std::array nested = {"control"_sd, "min"_sd};
    const auto doc = removeNestedField(getSampleDoc(), nested);
    if (GetParam() == SchemaViolationTestMode::ExpectFailOnDocumentInsert) {
        insertDoc(doc, ErrorCodes::DocumentValidationFailure);
    } else {
        insertDoc(doc);
        foregroundValidate(_nss,
                           _opCtx,
                           {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                           {_validateMode});
    }
}

TEST_P(TimeseriesCollectionValidationSchemaViolationTest, TimeseriesValidationMissingMax) {
    static constexpr std::array nested = {"control"_sd, "max"_sd};
    const auto doc = removeNestedField(getSampleDoc(), nested);
    if (GetParam() == SchemaViolationTestMode::ExpectFailOnDocumentInsert) {
        insertDoc(doc, ErrorCodes::DocumentValidationFailure);
    } else {
        insertDoc(doc);
        foregroundValidate(_nss,
                           _opCtx,
                           {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                           {_validateMode});
    }
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationIncorrectMinTimestamp) {
    const auto invalidDoc = std::invoke([] {
        static constexpr auto newMinIso = "1990-12-18T15:59:00Z"_sd;
        auto doc = getSampleDoc();
        auto oid = doc.getField("_id"_sd).OID();
        oid.setTimestamp(dateFromISOString(newMinIso).getValue().toMillisSinceEpoch());
        doc = replaceNestedField(doc, std::array{"_id"_sd}, oid);
        doc = replaceNestedField(doc,
                                 std::array{"control"_sd, "min"_sd, "date"_sd},
                                 dateFromISOString(newMinIso).getValue());
        return doc;
    });
    insertDoc(invalidDoc);
    // Timestamp and ID generate errors for mismatch between data and control block and for
    // improperly formatted bucket as the timestamp and _id are coupled. This test keeps the two
    // values aligned but mismatches the control and data block min values.
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationIncorrectMinIdField) {
    static constexpr std::array nested = {"control"_sd, "min"_sd, "_id"_sd};
    insertDoc(replaceNestedField(getSampleDoc(), nested, "xyz"_sd));
    // Timestamp and ID generate errors for mismatch between data and control block and for
    // improperly formatted bucket as the timestamp and _id are coupled.
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 2, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationIncorrectMinMeasurement) {
    static constexpr std::array nested = {"control"_sd, "min"_sd, "volume"_sd};
    insertDoc(replaceNestedField(getSampleDoc(), nested, 42));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationIncorrectMaxMeasurement) {
    static constexpr std::array nested = {"control"_sd, "max"_sd, "volume"_sd};
    insertDoc(replaceNestedField(getSampleDoc(), nested, 1'000'000));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest,
       TimeseriesValidationIncorrectMaxTimestampWithBucketSpanError) {
    static constexpr std::array nested = {"control"_sd, "max"_sd, "date"_sd};
    insertDoc(replaceNestedField(
        getSampleDoc(), nested, dateFromISOString("2025-12-18T15:59:00Z"_sd).getValue()));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMaxTimestampTooHigh) {
    static constexpr std::array nested = {"control"_sd, "max"_sd, "date"_sd};
    insertDoc(replaceNestedField(
        getSampleDoc(), nested, dateFromISOString("2021-12-18T16:00:00Z"_sd).getValue()));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMaxTimestampTooLow) {
    static constexpr std::array nested = {"control"_sd, "max"_sd, "date"_sd};
    insertDoc(replaceNestedField(
        getSampleDoc(), nested, dateFromISOString("2021-12-18T10:00:00Z"_sd).getValue()));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesControlSchema) {
    auto doc = getSampleDoc();
    const auto control = doc.getObjectField("control"_sd);
    const auto newMinObj = std::invoke([&control] {
        auto minObj = control.getField("min").Obj();
        // Rotate the fields by 1 then reinsert the object into the control block.
        BSONObjBuilder bob;
        for (auto it = std::next(minObj.begin(), 1); it != minObj.end(); ++it) {
            bob.append(*it);
        }
        bob.append(minObj.firstElement());
        return bob.obj();
    });
    insertDoc(replaceNestedField(doc, std::array{"control"_sd, "min"_sd}, newMinObj));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationIncorrectBucketObjectID) {
    static constexpr std::array nested = {"_id"_sd};
    const auto doc = getSampleDoc();
    auto oid = doc.getField("_id"_sd).OID();
    oid.setTimestamp(dateFromISOString("1990-12-18T15:59:00Z"_sd).getValue().toMillisSinceEpoch());
    insertDoc(replaceNestedField(doc, nested, oid));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMissingDate) {
    static constexpr std::array nested = {"data"_sd, "date"_sd};
    insertDoc(removeNestedField(getSampleDoc(), nested));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_P(TimeseriesCollectionValidationSchemaViolationTest, TimeseriesValidationIncorrectTimeField) {
    const auto doc = std::invoke([&] {
        BSONObj doc = getSampleDoc();

        const auto minDate = doc["control"]["min"]["date"];
        doc = replaceNestedField(doc,
                                 std::array{"control"_sd, "min"_sd, "date"_sd},
                                 minDate.Date(),
                                 replacementIncorrectTimeField);

        const auto maxDate = doc["control"]["max"]["date"];
        doc = replaceNestedField(doc,
                                 std::array{"control"_sd, "max"_sd, "date"_sd},
                                 maxDate.Date(),
                                 replacementIncorrectTimeField);

        doc = replaceNestedField(doc,
                                 std::array{"data"_sd, "date"_sd},
                                 doc["data"]["date"],
                                 replacementIncorrectTimeField);
        return doc;
    });
    if (GetParam() == SchemaViolationTestMode::ExpectFailOnDocumentInsert) {
        insertDoc(doc, ErrorCodes::DocumentValidationFailure);
    } else {
        insertDoc(doc);
        foregroundValidate(_nss,
                           _opCtx,
                           {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                           {_validateMode});
        if (testing::Test::HasFailure()) {
            // For documentary purposes
            ADD_FAILURE() << "Document inserted into collection: " << doc;
        };
    }
}


TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationIncorrectTimeFieldInDataOnly) {
    const auto doc = std::invoke([&] {
        BSONObj doc = getSampleDoc();
        doc = replaceNestedField(doc,
                                 std::array{"data"_sd, "date"_sd},
                                 doc["data"]["date"],
                                 "clearlyIncorrectReplacementTimeField"_sd);
        return doc;
    });
    insertDoc(doc);
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
    if (testing::Test::HasFailure()) {
        // For documentary purposes
        ADD_FAILURE() << "Document inserted into collection: " << doc;
    }
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMissingMeasurementClose) {
    static constexpr std::array nested = {"data"_sd, "close"_sd};
    insertDoc(removeNestedField(getSampleDoc(), nested));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMissingMeasurementFieldVolume) {
    static constexpr std::array nested = {"data"_sd, "volume"_sd};
    insertDoc(removeNestedField(getSampleDoc(), nested));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMismatchedMeasurementFieldClose) {
    insertDoc(getSampleDocMismatchedMeasurementField("close"_sd));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMismatchedMeasurementFieldVolume) {
    insertDoc(getSampleDocMismatchedMeasurementField("volume"_sd));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationCorruptData) {
    const auto corruptSampleDoc = std::invoke([] {
        const BSONObj bsonOrig = getSampleDoc();
        std::vector<char> sampleDocBuf;
        const auto bsonSpan = std::span(bsonOrig.objdata(), bsonOrig.objsize());
        std::ranges::copy(bsonSpan, std::back_inserter(sampleDocBuf));

        // BFS to handle nesting, for the first compressed column object found, remove some
        // bytes to corrupt the data.
        for (std::deque<BSONElement> q{bsonOrig.begin(), bsonOrig.end()}; !q.empty();) {
            const auto elem = q.front();
            q.pop_front();
            if (elem.isBinData(BinDataType::Column)) {
                int sz{0};
                const auto binData = elem.binData(sz);
                // Subtype should be the preceding byte.
                ASSERT_EQ(stdx::to_underlying(BinDataType::Column), binData[-1])
                    << "Something went wrong, the binary type should be Column data";
                const ptrdiff_t dist = (binData - bsonOrig.objdata());
                // Remove a chunk from the data buffer.
                const auto beg = sampleDocBuf.begin() + dist;
                const auto end = sampleDocBuf.begin() + dist + (sz / 2 + 1);
                sampleDocBuf.erase(beg, end);
                ASSERT_LT(sampleDocBuf.size(), size_t(bsonOrig.objsize()));
                break;  // Mangling the buffer will invalidate elements still in the queue, so
                        // break on the first element to be changed.
            } else if (elem.isABSONObj()) {
                const auto bsonObj = elem.Obj();
                q.insert(q.end(), bsonObj.begin(), bsonObj.end());
            }
        }
        BSONObj bson(sampleDocBuf.data());
        return bson.getOwned();  // Make sure this is owned as sampleDocBuf will go out of scope.
    });

    ASSERT_OK(storageInterface()->createCollection(_opCtx, _nss, _options));
    {
        WriteUnitOfWork wuow(_opCtx);
        const AutoGetCollection coll(_opCtx, _nss, MODE_IX);
        ASSERT_TRUE(coll->isTimeseriesCollection());
        ASSERT_OK(Helpers::insert(_opCtx, *coll, corruptSampleDoc));
        wuow.commit();
    }
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = true, .numRecords = 1, .numErrors = 0, .numWarnings = 2},
                       {_validateMode});
}

TEST_P(TimeseriesCollectionValidationSchemaViolationTest,
       TimeseriesValidationNonTimeseriesDocument) {
    const auto doc = BSON("_id" << "x");
    if (GetParam() == SchemaViolationTestMode::ExpectFailOnDocumentInsert) {
        insertDoc(doc,
                  ErrorCodes::DocumentValidationFailure);  // insert a non-timeseries document
    } else {
        insertDoc(doc);
        foregroundValidate(_nss,
                           _opCtx,
                           {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                           {CollectionValidation::ValidateMode::kForegroundFullCheckBSON});
    }
}


TEST_F(TimeseriesCollectionValidationTest, ReportErrorsInExtendedRangeBookkeeping) {
    const auto doc = getExtendedTimeRangeSampleDoc();
    insertDoc(doc);
    {
        AutoGetCollection coll(_opCtx, _nss, MODE_IS);
        EXPECT_FALSE(coll->getRequiresTimeseriesExtendedRangeSupport());
    }
    foregroundValidate(
        _nss, _opCtx, {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0});
}

TEST_F(TimeseriesCollectionValidationTest, MayRequireExtendedRangeSupportExpectTrue) {
    const auto doc = getExtendedTimeRangeSampleDoc();
    insertDoc(doc);
    const auto* coll = CollectionCatalog::get(_opCtx)->lookupCollectionByNamespace(_opCtx, _nss);
    ASSERT_NE(coll, nullptr);
    EXPECT_TRUE(timeseries::collectionMayRequireExtendedRangeSupport(_opCtx, *coll));
}

TEST_F(TimeseriesCollectionValidationTest, MayRequireExtendedRangeSupportExpectFalse) {
    const auto doc = getSampleDoc();
    insertDoc(doc);
    const auto* coll = CollectionCatalog::get(_opCtx)->lookupCollectionByNamespace(_opCtx, _nss);
    ASSERT_NE(coll, nullptr);
    EXPECT_FALSE(timeseries::collectionMayRequireExtendedRangeSupport(_opCtx, *coll));
}

}  // namespace
}  // namespace mongo

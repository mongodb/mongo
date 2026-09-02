// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/collection_validation.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bson_validate.h"
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
#include "mongo/db/repl/internode_validation_hash_utils.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
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
#include "mongo/db/validate/validate_timeseries.h"
#include "mongo/unittest/server_parameter_guard.h"
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
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
    std::initializer_list<collection_validation::ValidateMode> modes =
        {collection_validation::ValidateMode::kForeground,
         collection_validation::ValidateMode::kForegroundFull,
         collection_validation::ValidateMode::kForegroundFullCheckBSON},
    collection_validation::RepairMode repairMode = collection_validation::RepairMode::kNone,
    ValidationVersion validationVersion = currentValidationVersion) {

    std::vector<ValidateResults> results;

    for (const auto mode : modes) {
        ValidateResults validateResults;
        EXPECT_EQ(ErrorCodes::OK,
                  collection_validation::validate(
                      opCtx,
                      nss,
                      collection_validation::ValidationOptions{
                          mode, repairMode, /*logDiagnostics=*/false, validationVersion},
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
        auto invalidBson = "\0\0\0\0\0"sv;
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

/**
 * Bypasses 'create' validation that would reject an invalid index name to inject an invalid name.
 */
NamespaceString createClusteredCollectionWithIndexName(OperationContext* opCtx,
                                                       repl::StorageInterface* storageInterface,
                                                       std::string_view indexName) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.clustered");

    ClusteredIndexSpec indexSpec;
    indexSpec.setKey(BSON("_id" << 1));
    indexSpec.setUnique(true);
    indexSpec.setName(std::string{indexName});

    CollectionOptions options;
    options.clusteredIndex =
        ClusteredCollectionInfo(std::move(indexSpec), false /* legacyFormat */);
    ASSERT_OK(storageInterface->createCollection(opCtx, nss, options));
    return nss;
}

TEST_F(CollectionValidationTest, ValidateClusteredIndexNameWithEmbeddedNulByte) {
    auto opCtx = operationContext();
    const auto nss =
        createClusteredCollectionWithIndexName(opCtx, storageInterface(), "leading\0trailing"sv);
    const auto allResults = foregroundValidate(
        nss, opCtx, {.valid = false, .numRecords = 0, .numErrors = 1, .numWarnings = 0});
    for (const auto& results : allResults) {
        EXPECT_THAT(results.getErrors(),
                    testing::ElementsAre(
                        testing::AllOf(testing::HasSubstr("The clustered index name is not valid"),
                                       testing::HasSubstr("index name cannot contain NUL bytes"))));
    }
}

TEST_F(CollectionValidationTest, ValidateClusteredIndexNameEmpty) {
    auto opCtx = operationContext();
    const auto nss = createClusteredCollectionWithIndexName(opCtx, storageInterface(), ""sv);
    const auto allResults = foregroundValidate(
        nss, opCtx, {.valid = false, .numRecords = 0, .numErrors = 1, .numWarnings = 0});
    for (const auto& results : allResults) {
        EXPECT_THAT(results.getErrors(),
                    testing::ElementsAre(
                        testing::AllOf(testing::HasSubstr("The clustered index name is not valid"),
                                       testing::HasSubstr("index name cannot be empty"))));
    }
}

TEST_F(CollectionValidationTest, ValidateClusteredIndexNameValid) {
    auto opCtx = operationContext();
    const auto nss =
        createClusteredCollectionWithIndexName(opCtx, storageInterface(), "myClusteredIndex"sv);
    foregroundValidate(
        nss, opCtx, {.valid = true, .numRecords = 0, .numErrors = 0, .numWarnings = 0});
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
                       {collection_validation::ValidateMode::kForegroundFullEnforceFastCount});
}

// Verify calling validate() with enforceFastSize=true.
TEST_F(CollectionValidationTest, ValidateEnforceFastSize) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       {.valid = true,
                        .numRecords = insertDataRange(opCtx, 0, 5),
                        .numInvalidDocuments = 0,
                        .numErrors = 0},
                       {collection_validation::ValidateMode::kForegroundFullEnforceFastSize});
}

// Verify calling validate() with enforceFastCount=true and enforceFastSize=true.
TEST_F(CollectionValidationTest, ValidateEnforceFastCountAndSize) {
    auto opCtx = operationContext();
    foregroundValidate(
        kNss,
        opCtx,
        {.valid = true,
         .numRecords = insertDataRange(opCtx, 0, 5),
         .numInvalidDocuments = 0,
         .numErrors = 0},
        {collection_validation::ValidateMode::kForegroundFullEnforceFastCountAndSize});
}

// Verify that a record store which is empty from the traversal's point of view does not have its
// zeroed traversal counters compared against the fast count. A record store can look empty to
// validate's snapshot while the fast count still reflects records in some cases.
TEST_F(CollectionValidationTest, ValidateEnforceFastCountSkippedWhenTraversalSeesNoRecords) {
    auto opCtx = operationContext();

    // Skew the fast count so it disagrees with the empty record store, standing in for records that
    // exist but are invisible to this snapshot.
    {
        const AutoGetCollection coll(opCtx, kNss, MODE_X);
        coll->getRecordStore()->updateStatsAfterRepair(/*numRecords=*/2, /*dataSize=*/64);
        ASSERT_EQ(2, coll->latestSizeCount(opCtx).count);
    }

    foregroundValidate(
        kNss,
        opCtx,
        {.valid = true,
         .numRecords = 0,
         .numInvalidDocuments = 0,
         .numErrors = 0,
         .numWarnings = 0},
        {collection_validation::ValidateMode::kForegroundFullEnforceFastCount,
         collection_validation::ValidateMode::kForegroundFullEnforceFastCountAndSize});
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
                       {collection_validation::ValidateMode::kForegroundCheckBSON});
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
                       {collection_validation::ValidateMode::kForegroundCheckBSON});
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
                       {collection_validation::ValidateMode::kForegroundCheckBSON});
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
                       {collection_validation::ValidateMode::kForegroundCheckBSON});
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
                       {collection_validation::ValidateMode::kForegroundCheckBSON});
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
        {collection_validation::ValidateMode::kForegroundFull});
}

/**
 * Waits for a parallel running collection validation operation to start and then hang at a
 * failpoint.
 *
 * A failpoint in the validate() code should have been set prior to calling this function.
 */
void waitUntilValidateFailpointHasBeenReached() {
    while (!collection_validation::getIsValidationPausedForTest()) {
        sleepmillis(100);  // a fairly arbitrary sleep period.
    }
    ASSERT(collection_validation::getIsValidationPausedForTest());
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
    ASSERT_THROWS_CODE(collection_validation::validateHashes({""}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(collection_validation::validateHashes({""}, /*equalLength=*/false),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesTooLong) {
    constexpr int kHashStringMaxLen = 64;
    ASSERT_DOES_NOT_THROW(collection_validation::validateHashes(
        {std::string(kHashStringMaxLen, 'A')}, /*equalLength=*/true));

    ASSERT_THROWS_CODE(collection_validation::validateHashes(
                           {std::string(kHashStringMaxLen + 1, 'A')}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(collection_validation::validateHashes(
                           {std::string(kHashStringMaxLen + 1, 'A')}, /*equalLength=*/false),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesDifferentLengths) {
    ASSERT_DOES_NOT_THROW(
        collection_validation::validateHashes({"AAA", "BBBB"}, /*equalLength=*/false));

    ASSERT_THROWS_CODE(collection_validation::validateHashes({"AAA", "BBBB"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesHexString) {
    ASSERT_THROWS_CODE(collection_validation::validateHashes({"NOTHEX"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(collection_validation::validateHashes({"NOTHEX"}, /*equalLength=*/false),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesDuplicates) {
    ASSERT_THROWS_CODE(collection_validation::validateHashes({"ABC", "ABC"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(
        collection_validation::validateHashes({"ABC", "ABCD", "A"}, /*equalLength=*/false),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(CollectionValidationTest, HashPrefixesCases) {
    constexpr int kHashStringMaxLen = 64;
    ASSERT_DOES_NOT_THROW(
        collection_validation::validateHashes({"AAA1", "BBB1", "CCC1"}, /*equalLength=*/true));
    ASSERT_DOES_NOT_THROW(collection_validation::validateHashes(
        {std::string(kHashStringMaxLen, 'A')}, /*equalLength=*/true));

    ASSERT_THROWS_CODE(collection_validation::validateHashes({"a"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(collection_validation::validateHashes({"AAA", "BBBB"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(collection_validation::validateHashes({"nothex"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(collection_validation::validateHashes({"AAA", "AAA"}, /*equalLength=*/true),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(
        collection_validation::validateHashes({"abcd", "a", "ABCDEF"}, /*equalLength=*/true),
        DBException,
        ErrorCodes::InvalidOptions);
}

// Documents inserted by the slicing tests below, and the per-slice target that splits them across
// many slices.
static constexpr int kCountDocs{1'000};
static constexpr int64_t kTargetRecordsPerSlice{10};

TEST_F(CollectionValidationTest, LargeCollectionSlicesOnParallelExecution) {
    auto opCtx = operationContext();
    ASSERT_EQ(kCountDocs, insertDataRange(opCtx, 0, kCountDocs));
    ValidateResults results;
    collection_validation::ValidationOptions opts(
        collection_validation::ValidateMode::kForeground,
        collection_validation::RepairMode::kNone,
        /*logDiagnostics=*/false,
        currentValidationVersion,
        /*verifyConfigurationOverride=*/boost::none,
        /*readTimestamp=*/boost::none,
        /*hashPrefixes=*/boost::none,
        /*revealHashedIds=*/boost::none,
        /*targetRecordsPerRecordStoreSlice=*/kTargetRecordsPerSlice);
    ASSERT_OK(collection_validation::validate(opCtx, kNss, opts, &results));
    // Do not strictly validate on a particular count, simply check that it's more than one, and the
    // value is populated.
    ASSERT_GT(results.getNumRecordStoreSlices().value_or(-1), 1)
        << "Expected more than one record store slice";
}

TEST_F(CollectionValidationTest, LargeCollectionDoesNotSliceOnSerialExecution) {
    auto opCtx = operationContext();
    ASSERT_EQ(kCountDocs, insertDataRange(opCtx, 0, kCountDocs));
    collection_validation::ValidationOptions opts(collection_validation::ValidateMode::kForeground,
                                                  collection_validation::RepairMode::kNone,
                                                  /*logDiagnostics=*/false,
                                                  currentValidationVersion,
                                                  /*verifyConfigurationOverride=*/boost::none,
                                                  /*readTimestamp=*/boost::none,
                                                  /*hashPrefixes=*/boost::none,
                                                  /*revealHashedIds=*/boost::none,
                                                  /*targetRecordsPerRecordStoreSlice=*/boost::none);
    ValidateResults results;
    ASSERT_OK(collection_validation::validate(opCtx, kNss, opts, &results));
    // Ensure that only one slice was run, or the value is unpopulated.
    ASSERT_EQ(results.getNumRecordStoreSlices().value_or(1), 1);
}

TEST_F(CollectionValidationTest, ParallelTraversalAgreesWithSerialTraversal) {
    auto opCtx = operationContext();
    ASSERT_EQ(kCountDocs, insertDataRange(opCtx, 0, kCountDocs));

    auto validateWithSliceTarget = [&](boost::optional<int64_t> targetRecordsPerSlice) {
        collection_validation::ValidationOptions opts(
            collection_validation::ValidateMode::kForeground,
            collection_validation::RepairMode::kNone,
            /*logDiagnostics=*/false,
            currentValidationVersion,
            /*verifyConfigurationOverride=*/boost::none,
            /*readTimestamp=*/boost::none,
            /*hashPrefixes=*/boost::none,
            /*revealHashedIds=*/boost::none,
            /*targetRecordsPerRecordStoreSlice=*/targetRecordsPerSlice);
        ValidateResults results;
        ASSERT_OK(collection_validation::validate(opCtx, kNss, opts, &results));
        return results;
    };

    const auto serialResults = validateWithSliceTarget(boost::none);
    ASSERT_TRUE(serialResults.isValid());
    ASSERT_EQ(kCountDocs, serialResults.getNumRecords().value_or(-1));

    // Slicing must not change what validation reports. The slice totals are cross-checked against a
    // separate counting traversal on the caller's snapshot, so a gap or an overlap between slices
    // -- or a worker reading from a snapshot other than the caller's -- surfaces as a record count
    // mismatch and an invalid result.
    const auto parallelResults = validateWithSliceTarget(kTargetRecordsPerSlice);
    ASSERT_TRUE(parallelResults.isValid());
    ASSERT_EQ(kCountDocs, parallelResults.getNumRecords().value_or(-1));
    ASSERT_GT(parallelResults.getNumRecordStoreSlices().value_or(-1), 1);
}

// Every in-flight slice holds an index consistency bucket array of its own until the results are
// merged, so the slice count must stay bounded no matter how small the per-slice target is
// relative to the collection.
TEST_F(CollectionValidationTest, SliceCountIsCappedRegardlessOfTarget) {
    static constexpr int kMaxSlices{4};
    unittest::ServerParameterGuard maxSlicesGuard{"validateParallelMaxRecordStoreSlices",
                                                  kMaxSlices};

    auto opCtx = operationContext();
    ASSERT_EQ(kCountDocs, insertDataRange(opCtx, 0, kCountDocs));

    // A target of one record per slice would ask for kCountDocs slices without the cap.
    collection_validation::ValidationOptions opts(collection_validation::ValidateMode::kForeground,
                                                  collection_validation::RepairMode::kNone,
                                                  /*logDiagnostics=*/false,
                                                  currentValidationVersion,
                                                  /*verifyConfigurationOverride=*/boost::none,
                                                  /*readTimestamp=*/boost::none,
                                                  /*hashPrefixes=*/boost::none,
                                                  /*revealHashedIds=*/boost::none,
                                                  /*targetRecordsPerRecordStoreSlice=*/1);
    ValidateResults results;
    ASSERT_OK(collection_validation::validate(opCtx, kNss, opts, &results));

    ASSERT_TRUE(results.isValid());
    ASSERT_EQ(kCountDocs, results.getNumRecords().value_or(-1));
    const auto numSlices = results.getNumRecordStoreSlices().value_or(1);
    ASSERT_GT(numSlices, 1);
    ASSERT_LTE(numSlices, kMaxSlices);
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
                           std::span<const std::string_view> nestedFieldNames,
                           const T& replacement,
                           boost::optional<std::string_view> replacementFieldName = boost::none) {
    invariant(!nestedFieldNames.empty());
    const std::string_view cur = nestedFieldNames.front();
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

BSONObj removeNestedField(const BSONObj& bson, std::span<const std::string_view> nestedFieldNames) {
    invariant(!nestedFieldNames.empty());
    const std::string_view cur = nestedFieldNames.front();
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
        : CatalogTestFixture(std::move(options)),
          _allowCorruptTimeseriesBuckets("timeseriesDisableStrictBucketValidator", true) {
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

    static BSONObj getJustPastEpochMaxDoc() {
        // Note that this simulates "seconds" granularity for a bucket just past the epoch time max.
        // The server will round this document down to the date in "control.min.date"
        // In mongosh, the document can be reproduced by:
        // > db.createCollection("ts", {timeseries: {timeField: "date"}})
        // > db.ts.insertOne({date: ISODate("2038-01-17T03:14:08Z"), data: 42})
        // > EJSON.stringify(db.system.buckets.ts.findOne(), null, 4) // requires mongosh
        return ::mongo::fromjson(R"BSON(
            {
                "_id": {
                    "$oid": "7ffd5cf829fd87c86e0fda70"
                },
                "control": {
                    "version": 2,
                    "min": {
                        "date": {
                            "$date": "2038-01-17T03:14:00Z"
                        },
                        "data": 42,
                        "_id": {
                            "$oid": "69b86f31cf1a6157b5542e76"
                        }
                    },
                    "max": {
                        "date": {
                            "$date": "2038-01-17T03:14:08Z"
                        },
                        "data": 42,
                        "_id": {
                            "$oid": "69b86f31cf1a6157b5542e76"
                        }
                    },
                    "count": 1
                },
                "data": {
                    "data": {
                        "$binary": {
                            "base64": "EAAqAAAAAA==",
                            "subType": "07"
                        }
                    },
                    "date": {
                        "$binary": {
                            "base64": "CQAASLP18wEAAAA=",
                            "subType": "07"
                        }
                    },
                    "_id": {
                        "$binary": {
                            "base64": "BwBpuG8xzxphV7VULnYA",
                            "subType": "07"
                        }
                    }
                }
            })BSON");
    }

    static std::vector<BSONObj> getAllSampleDocs() {
        return {getSampleDoc(), getVersion3ControlSampleDoc(), getExtendedTimeRangeSampleDoc()};
    };

    static constexpr auto replacementIncorrectTimeField = "t"sv;

    void insertDocs(std::span<const BSONObj> docs, ErrorCodes::Error expected = ErrorCodes::OK) {
        ASSERT_OK(storageInterface()->createCollection(_opCtx, _nss, _options));
        WriteUnitOfWork wuow(_opCtx);
        const AutoGetCollection coll(_opCtx, _nss, MODE_IX);
        ASSERT_TRUE(coll->isTimeseriesCollection());
        EXPECT_EQ(expected, Helpers::insert(_opCtx, *coll, docs));
        wuow.commit();
    }

    void insertDoc(BSONObj doc, ErrorCodes::Error expected = ErrorCodes::OK) {
        insertDocs(std::array{doc}, expected);
    }


    static BSONObj getSampleDocMismatchedMeasurementField(std::string_view measurementField) {
        const auto origBson = getSampleDoc();
        const BSONColumn colData(origBson.getObjectField("data"sv).firstElement());

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

        const std::vector<std::string_view> nested = {"data"sv, measurementField};
        return replaceNestedField(origBson, nested, bcb.finalize());
    }

    NamespaceString _nss;
    OperationContext* _opCtx{nullptr};
    CollectionOptions _options;
    collection_validation::ValidateMode _validateMode{
        collection_validation::ValidateMode::kForeground};
    unittest::ServerParameterGuard _allowCorruptTimeseriesBuckets;

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _options.uuid = UUID::gen();
        _options.timeseries = TimeseriesOptions(/*timeField*/ "date");
        _options.timeseries->setBucketRoundingSeconds(60);
        _options.timeseries->setBucketMaxSpanSeconds(60 * 60 * 24);
        _options.timeseries->setMetaField("ticker"sv);
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
    static constexpr std::array nested = {"control"sv, "count"sv};
    insertDoc(replaceNestedField(getSampleDoc(), nested, 4));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_P(TimeseriesCollectionValidationSchemaViolationTest, TimeseriesValidationMissingMin) {
    static constexpr std::array nested = {"control"sv, "min"sv};
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
    static constexpr std::array nested = {"control"sv, "max"sv};
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
        static constexpr auto newMinIso = "1990-12-18T15:59:00Z"sv;
        auto doc = getSampleDoc();
        auto oid = doc.getField("_id"sv).OID();
        oid.setTimestamp(dateFromISOString(newMinIso).getValue().toMillisSinceEpoch());
        doc = replaceNestedField(doc, std::array{"_id"sv}, oid);
        doc = replaceNestedField(doc,
                                 std::array{"control"sv, "min"sv, "date"sv},
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
    static constexpr std::array nested = {"control"sv, "min"sv, "_id"sv};
    insertDoc(replaceNestedField(getSampleDoc(), nested, "xyz"sv));
    // Timestamp and ID generate errors for mismatch between data and control block and for
    // improperly formatted bucket as the timestamp and _id are coupled.
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 2, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationIncorrectMinMeasurement) {
    static constexpr std::array nested = {"control"sv, "min"sv, "volume"sv};
    insertDoc(replaceNestedField(getSampleDoc(), nested, 42));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationIncorrectMaxMeasurement) {
    static constexpr std::array nested = {"control"sv, "max"sv, "volume"sv};
    insertDoc(replaceNestedField(getSampleDoc(), nested, 1'000'000));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest,
       TimeseriesValidationIncorrectMaxTimestampWithBucketSpanError) {
    static constexpr std::array nested = {"control"sv, "max"sv, "date"sv};
    insertDoc(replaceNestedField(
        getSampleDoc(), nested, dateFromISOString("2025-12-18T15:59:00Z"sv).getValue()));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMaxTimestampTooHigh) {
    static constexpr std::array nested = {"control"sv, "max"sv, "date"sv};
    insertDoc(replaceNestedField(
        getSampleDoc(), nested, dateFromISOString("2021-12-18T16:00:00Z"sv).getValue()));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMaxTimestampTooLow) {
    static constexpr std::array nested = {"control"sv, "max"sv, "date"sv};
    insertDoc(replaceNestedField(
        getSampleDoc(), nested, dateFromISOString("2021-12-18T10:00:00Z"sv).getValue()));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesControlSchema) {
    auto doc = getSampleDoc();
    const auto control = doc.getObjectField("control"sv);
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
    insertDoc(replaceNestedField(doc, std::array{"control"sv, "min"sv}, newMinObj));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationIncorrectBucketObjectID) {
    static constexpr std::array nested = {"_id"sv};
    const auto doc = getSampleDoc();
    auto oid = doc.getField("_id"sv).OID();
    oid.setTimestamp(dateFromISOString("1990-12-18T15:59:00Z"sv).getValue().toMillisSinceEpoch());
    insertDoc(replaceNestedField(doc, nested, oid));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMissingDate) {
    static constexpr std::array nested = {"data"sv, "date"sv};
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
                                 std::array{"control"sv, "min"sv, "date"sv},
                                 minDate.Date(),
                                 replacementIncorrectTimeField);

        const auto maxDate = doc["control"]["max"]["date"];
        doc = replaceNestedField(doc,
                                 std::array{"control"sv, "max"sv, "date"sv},
                                 maxDate.Date(),
                                 replacementIncorrectTimeField);

        doc = replaceNestedField(doc,
                                 std::array{"data"sv, "date"sv},
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
                                 std::array{"data"sv, "date"sv},
                                 doc["data"]["date"],
                                 "clearlyIncorrectReplacementTimeField"sv);
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
    static constexpr std::array nested = {"data"sv, "close"sv};
    insertDoc(removeNestedField(getSampleDoc(), nested));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMissingMeasurementFieldVolume) {
    static constexpr std::array nested = {"data"sv, "volume"sv};
    insertDoc(removeNestedField(getSampleDoc(), nested));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMismatchedMeasurementFieldClose) {
    insertDoc(getSampleDocMismatchedMeasurementField("close"sv));
    foregroundValidate(_nss,
                       _opCtx,
                       {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0},
                       {_validateMode});
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationMismatchedMeasurementFieldVolume) {
    insertDoc(getSampleDocMismatchedMeasurementField("volume"sv));
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
                           {collection_validation::ValidateMode::kForegroundFullCheckBSON});
    }
}

TEST_F(TimeseriesCollectionValidationTest, MayRequireExtendedRangeSupportExpectTrue) {
    const auto docs = std::array{getExtendedTimeRangeSampleDoc(), getSampleDoc()};
    insertDocs(docs);
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

TEST_F(TimeseriesCollectionValidationTest, ReportErrorsInExtendedRangeBookkeeping) {
    const auto doc = getExtendedTimeRangeSampleDoc();
    insertDoc(doc);
    {
        const AutoGetCollection coll(_opCtx, _nss, MODE_IS);
        EXPECT_FALSE(coll->getRequiresTimeseriesExtendedRangeSupport());
    }
    foregroundValidate(
        _nss, _opCtx, {.valid = false, .numRecords = 1, .numErrors = 1, .numWarnings = 0});
}

TEST_F(TimeseriesCollectionValidationTest, ValidationOfDocumentJustPastEpochMax) {
    const auto doc = getJustPastEpochMaxDoc();
    insertDoc(doc);
    {
        const AutoGetCollection coll(_opCtx, _nss, MODE_IS);
        EXPECT_FALSE(coll->getRequiresTimeseriesExtendedRangeSupport())
            << "Do not set the flag for this test";
    }
    foregroundValidate(
        _nss, _opCtx, {.valid = true, .numRecords = 1, .numErrors = 0, .numWarnings = 0});
}

TEST_F(TimeseriesCollectionValidationTest, ReportWarningForV3BucketWithMeasurementsInOrder) {
    static constexpr std::array version = {"control"sv, "version"sv};
    insertDoc(replaceNestedField(getSampleDoc(), version, 3));
    foregroundValidate(
        _nss, _opCtx, {.valid = true, .numRecords = 1, .numErrors = 0, .numWarnings = 1});
}

TEST_F(TimeseriesCollectionValidationTest, ReportInvalidBSONColumnReason) {
    // 0xF1 = interleaved start byte; empty BSON object {} follows as the reference object.
    // BSONColumn iteration immediately uasserts InvalidBSONColumn because the empty reference
    // object has no fields, making interleaved.states empty.
    // V2_Column wraps all column errors as NonConformantBSON; V1_Original lets InvalidBSONColumn
    // propagate directly, which is the path under test (includeReason=true in validateRecord).
    const char kInvalidColumnBytes[] = "\xF1\x05\x00\x00\x00\x00";
    const BSONBinData invalidColumn{
        kInvalidColumnBytes, sizeof(kInvalidColumnBytes) - 1, BinDataType::Column};
    static constexpr std::array dataIdField = {"data"sv, "_id"sv};
    insertDoc(replaceNestedField(getSampleDoc(), dataIdField, invalidColumn));

    const auto results = foregroundValidate(
        _nss,
        _opCtx,
        {.valid = false, .numRecords = 1, .numInvalidDocuments = 1, .numErrors = 1},
        {_validateMode},
        collection_validation::RepairMode::kNone,
        V1_Original);

    ASSERT_EQ(results.size(), 1U);
    ASSERT_THAT(*results.front().getErrors().begin(), ::testing::HasSubstr("InvalidBSONColumn"));
}

TEST_F(TimeseriesCollectionValidationTest, TimeseriesValidationFixedBucketingInconsistency) {
    unittest::ServerParameterGuard fixedBucketingCatalogController(
        "featureFlagFixedBucketingCatalog", true);

    // Collection has fixedBucketing=true and hours granularity. The sample doc has
    // control.min.date=2021-12-18T15:55:00Z. Under hours granularity a bucket opened for
    // this timestamp would have control.min=15:00:00, but 15:55:00 implies it was created
    // under finer granularity, contradicting fixedBucketing=true.
    _options.timeseries->setGranularity(BucketGranularityEnum::Hours);
    _options.timeseries->setBucketRoundingSeconds(3600);
    _options.timeseries->setBucketMaxSpanSeconds(3600);
    _options.timeseries->setFixedBucketing(true);

    insertDoc(getSampleDoc());

    auto results = foregroundValidate(
        _nss, _opCtx, {.valid = false, .numRecords = 1, .numErrors = 1}, {_validateMode});
    ASSERT(results[0].getErrors().count(
        std::string{collection_validation::kTimeseriesFixedBucketingInconsistencyReason}));
}

/**
 * Records the per-document validation hashes the way disaggregated storage does, which is what
 * makes validate accumulate an XXH3 collection hash.
 */
class ContinuousInternodeValidationProvider : public rss::AttachedPersistenceProvider {
public:
    bool shouldUseContinuousInternodeValidation() const override {
        return true;
    }
};

// Enables both the replicated metadata system and continuous internode validation, so validate
// accumulates a collection hash and has a persisted one to compare it against.
class ComparableHashProvider
    : public replicated_fast_count::test_helpers::ReplicatedFastCountTestPersistenceProvider {
public:
    bool shouldUseContinuousInternodeValidation() const override {
        return true;
    }
};

class CollectionHashComparisonTest : public CollectionValidationTest {
protected:
    CollectionHashComparisonTest()
        : CollectionValidationTest(
              Options{}.setPersistenceProvider(std::make_unique<ComparableHashProvider>())) {}

    // Creates the replicated metadata containers and points the manager's stores at them, the way
    // startup does. Without the second step the manager keeps its default collection-backed
    // stores, whose read path expects a collection that does not exist here.
    void createFastCountContainers() {
        ASSERT_OK(createInternalFastCountContainers(operationContext(),
                                                    NamespaceString::kAdminCommandNamespace,
                                                    ident::kFastCountMetadataStore,
                                                    KeyFormat::String,
                                                    ident::kFastCountMetadataStoreTimestamps,
                                                    KeyFormat::Long,
                                                    /*writeToOplog=*/false));

        KVEngine* engine = operationContext()->getServiceContext()->getStorageEngine()->getEngine();
        replicated_fast_count::ReplicatedFastCountManager::get(getServiceContext())
            .initializeContainerStores(
                engine->getRecordStore(operationContext(),
                                       NamespaceString::kAdminCommandNamespace,
                                       ident::kFastCountMetadataStore,
                                       RecordStore::Options{.keyFormat = KeyFormat::String},
                                       /*uuid=*/boost::none),
                engine->getRecordStore(operationContext(),
                                       NamespaceString::kAdminCommandNamespace,
                                       ident::kFastCountMetadataStoreTimestamps,
                                       RecordStore::Options{.keyFormat = KeyFormat::Long},
                                       /*uuid=*/boost::none));

        // validate() requires an inactive recovery unit when it sets its prepare conflict
        // behavior, and the setup above leaves a snapshot open.
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    }

    // The point a seeded hash is valid as of.
    static inline const Timestamp kPersistedAt = Timestamp(1, 1);

    // Seeds the replicated metadata system with a persisted hash for 'uuid', standing in for what
    // a checkpoint flush would have written.
    void seedPersistedHash(const UUID& uuid,
                           boost::optional<int64_t> hash,
                           bool writeBackingOplogEntry = true) {
        auto& manager = replicated_fast_count::ReplicatedFastCountManager::get(getServiceContext());
        auto [sizeCountStore, timestampStore] = manager.getSizeCountStores_ForTest();
        replicated_fast_count::test_helpers::insertSizeCountEntry(
            operationContext(),
            *sizeCountStore,
            uuid,
            replicated_fast_count::SizeCountStore::Entry{
                .timestamp = kPersistedAt, .size = 0, .count = 0, .hash = hash});

        if (!writeBackingOplogEntry) {
            shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
            return;
        }

        // A valid-as-of names the last oplog entry the accumulator consumed, so that record has to
        // exist. The scan seeks past it exclusively, so it contributes nothing itself.
        replicated_fast_count::test_helpers::writeToOplog(
            operationContext(),
            replicated_fast_count::test_helpers::makeOplogEntry(
                kPersistedAt,
                replicated_fast_count::test_helpers::NsAndUUID{kNss, uuid},
                repl::OpTypeEnum::kInsert,
                /*sizeDelta=*/0));
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    }

    // Seeds the store's global valid-as-of, the point the accumulator has processed the oplog up
    // to across all collections, along with the oplog record it names.
    void seedGlobalValidAsOf(const UUID& uuid, Timestamp ts) {
        auto& manager = replicated_fast_count::ReplicatedFastCountManager::get(getServiceContext());
        auto [sizeCountStore, timestampStore] = manager.getSizeCountStores_ForTest();
        replicated_fast_count::test_helpers::insertSizeCountTimestamp(
            operationContext(), *timestampStore, ts);
        replicated_fast_count::test_helpers::writeToOplog(
            operationContext(),
            replicated_fast_count::test_helpers::makeOplogEntry(
                ts,
                replicated_fast_count::test_helpers::NsAndUUID{kNss, uuid},
                repl::OpTypeEnum::kInsert,
                /*sizeDelta=*/0,
                /*hash=*/0));
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    }

    UUID collectionUuid() {
        const AutoGetCollection coll(operationContext(), kNss, MODE_IS);
        return coll->uuid();
    }

    void insertDocs(const std::vector<BSONObj>& docs) {
        const AutoGetCollection coll(operationContext(), kNss, MODE_IX);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), *coll, docs));
        wuow.commit();
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    }

    static uint64_t xxh3Of(const std::vector<BSONObj>& docs) {
        uint64_t hash = 0;
        for (const auto& doc : docs) {
            hash ^= static_cast<uint64_t>(repl::computeDocValidationHash(doc));
        }
        return hash;
    }

    ValidateResults hashValidate() {
        ValidateResults results;
        EXPECT_EQ(ErrorCodes::OK,
                  collection_validation::validate(
                      operationContext(),
                      kNss,
                      collection_validation::ValidationOptions{
                          collection_validation::ValidateMode::kCollectionHash,
                          collection_validation::RepairMode::kNone,
                          /*logDiagnostics=*/false},
                      &results));
        return results;
    }
};

// Probe: reports how far the comparison gets with the replicated metadata system enabled.
TEST_F(CollectionHashComparisonTest, ReportsNoPersistedEntryBeforeAnythingIsFlushed) {
    createFastCountContainers();
    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "noPersistedEntry");
    EXPECT_FALSE(results.getExpectedXxh3CollectionHash().has_value());
}

TEST_F(CollectionHashComparisonTest, MatchesWhenThePersistedHashAgrees) {
    createFastCountContainers();
    const std::vector<BSONObj> docs = {BSON("_id" << 1), BSON("_id" << 2)};
    insertDocs(docs);
    seedPersistedHash(collectionUuid(), static_cast<int64_t>(xxh3Of(docs)));

    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "matched");
    EXPECT_TRUE(results.getWarnings().empty());
    EXPECT_TRUE(results.isValid());
}

TEST_F(CollectionHashComparisonTest, FoldsInOplogEntriesWrittenSinceTheHashWasPersisted) {
    createFastCountContainers();

    // Stands in for a collection whose hash was persisted when it held 'flushed', after which
    // 'sinceFlush' was written. The persisted hash is stale by exactly that document, so the
    // comparison only holds if validate replays the oplog and folds it back in.
    const std::vector<BSONObj> flushed = {BSON("_id" << 1), BSON("_id" << 2)};
    const BSONObj sinceFlush = BSON("_id" << 3);
    insertDocs(flushed);
    insertDocs({sinceFlush});

    const auto uuid = collectionUuid();
    seedPersistedHash(uuid, static_cast<int64_t>(xxh3Of(flushed)));

    // The entry is persisted as of Timestamp(1, 1), so this lands in the replayed range.
    replicated_fast_count::test_helpers::writeToOplog(
        operationContext(),
        replicated_fast_count::test_helpers::makeOplogEntry(
            Timestamp(2, 1),
            replicated_fast_count::test_helpers::NsAndUUID{kNss, uuid},
            repl::OpTypeEnum::kInsert,
            /*sizeDelta=*/sinceFlush.objsize(),
            repl::computeDocValidationHash(sinceFlush)));
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "matched");
    EXPECT_TRUE(results.getWarnings().empty());

    // Without the replay the expected value would still be the stale persisted hash.
    ASSERT_TRUE(results.getExpectedXxh3CollectionHash().has_value());
    EXPECT_NE(static_cast<uint64_t>(*results.getExpectedXxh3CollectionHash()), xxh3Of(flushed));
}

TEST_F(CollectionHashComparisonTest, ReplaysFromTheGlobalValidAsOfNotTheEntrysOwn) {
    createFastCountContainers();
    const BSONObj doc = BSON("_id" << 1);
    insertDocs({doc});

    const auto uuid = collectionUuid();

    // The entry's hash already accounts for 'doc', and its own valid-as-of is old. The global
    // valid-as-of is later, which is the normal state for a collection that has not changed since
    // an earlier checkpoint.
    seedPersistedHash(uuid, static_cast<int64_t>(xxh3Of({doc})));
    seedGlobalValidAsOf(uuid, Timestamp(5, 1));

    // The write that produced that hash still sits in the oplog between the two timestamps.
    // Seeking from the entry's own valid-as-of would replay it a second time, and XOR being its
    // own inverse would cancel it out into a mismatch on a healthy collection.
    replicated_fast_count::test_helpers::writeToOplog(
        operationContext(),
        replicated_fast_count::test_helpers::makeOplogEntry(
            Timestamp(2, 1),
            replicated_fast_count::test_helpers::NsAndUUID{kNss, uuid},
            repl::OpTypeEnum::kInsert,
            /*sizeDelta=*/doc.objsize(),
            repl::computeDocValidationHash(doc)));
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "matched");
    ASSERT_TRUE(results.getExpectedXxh3CollectionHash().has_value());
    EXPECT_EQ(static_cast<uint64_t>(*results.getExpectedXxh3CollectionHash()), xxh3Of({doc}));
}

TEST_F(CollectionHashComparisonTest, ReportsNoPersistedHashWhenTheEntryCarriesNone) {
    createFastCountContainers();
    insertDocs({BSON("_id" << 1)});

    // An entry that predates hash validation, or whose contributions could not all be accounted
    // for, holds size and count but no hash. Absence is sticky, so this is the permanent state for
    // such a collection rather than a transient one.
    seedPersistedHash(collectionUuid(), boost::none);

    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "noPersistedHash");
    EXPECT_FALSE(results.getExpectedXxh3CollectionHash().has_value());
    EXPECT_TRUE(results.getWarnings().empty());
}

TEST_F(CollectionHashComparisonTest, SkipsWhenTheCallerPinsAReadTimestamp) {
    createFastCountContainers();
    const std::vector<BSONObj> docs = {BSON("_id" << 1)};
    insertDocs(docs);
    seedPersistedHash(collectionUuid(), static_cast<int64_t>(xxh3Of(docs)));

    // The scan would read an earlier instant than the replayed oplog describes, so the two sides
    // would be measuring different moments and could disagree on a healthy collection.
    ValidateResults results;
    EXPECT_EQ(
        ErrorCodes::OK,
        collection_validation::validate(operationContext(),
                                        kNss,
                                        collection_validation::ValidationOptions{
                                            collection_validation::ValidateMode::kCollectionHash,
                                            collection_validation::RepairMode::kNone,
                                            /*logDiagnostics=*/false,
                                            currentValidationVersion,
                                            /*verifyConfigurationOverride=*/boost::none,
                                            /*readTimestamp=*/
                                            operationContext()
                                                ->getServiceContext()
                                                ->getStorageEngine()
                                                ->getAllDurableTimestamp()},
                                        &results));

    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "pinnedReadTimestamp");
    EXPECT_FALSE(results.getExpectedXxh3CollectionHash().has_value());
}

TEST_F(CollectionHashComparisonTest, SkipsACollectionCreatedSinceTheLastCheckpoint) {
    createFastCountContainers();
    const BSONObj doc = BSON("_id" << 1);
    insertDocs({doc});

    const auto uuid = collectionUuid();

    // No per-collection entry, which is the state for a collection whose create is still in the
    // unflushed oplog range: either it was created since the last checkpoint, or an earlier
    // incarnation was dropped, that drop was flushed away (which removes the entry), and it was
    // then recreated. Both land here as kCreated with nothing persisted.
    //
    // The replayed delta could answer for the collection on its own, since a create restarts the
    // hash from zero. That is deliberately not done: it would compare the oplog against the data
    // rather than the persisted hash against the data, and reporting a match would claim more than
    // was checked.
    seedGlobalValidAsOf(uuid, kPersistedAt);

    replicated_fast_count::test_helpers::writeToOplog(
        operationContext(),
        replicated_fast_count::test_helpers::makeCreateOplogEntry(
            Timestamp(2, 1), replicated_fast_count::test_helpers::NsAndUUID{kNss, uuid}));
    replicated_fast_count::test_helpers::writeToOplog(
        operationContext(),
        replicated_fast_count::test_helpers::makeOplogEntry(
            Timestamp(3, 1),
            replicated_fast_count::test_helpers::NsAndUUID{kNss, uuid},
            repl::OpTypeEnum::kInsert,
            /*sizeDelta=*/doc.objsize(),
            repl::computeDocValidationHash(doc)));
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "noPersistedEntry");
    EXPECT_FALSE(results.getExpectedXxh3CollectionHash().has_value());
    EXPECT_TRUE(results.getWarnings().empty());
}

TEST_F(CollectionHashComparisonTest, ReportsIncompleteWhenAReplayedEntryCarriesNoHash) {
    createFastCountContainers();
    const std::vector<BSONObj> docs = {BSON("_id" << 1)};
    insertDocs(docs);
    seedPersistedHash(collectionUuid(), static_cast<int64_t>(xxh3Of(docs)));

    // An entry with a size delta but no per-document hash, which is what a write records when the
    // storage model was not accumulating hashes at the time. One such entry means not every write
    // in the range can be accounted for, so the folded value is incomplete and must not be
    // compared rather than compared while missing a contribution.
    replicated_fast_count::test_helpers::writeToOplog(
        operationContext(),
        replicated_fast_count::test_helpers::makeOplogEntry(
            Timestamp(2, 1),
            replicated_fast_count::test_helpers::NsAndUUID{kNss, collectionUuid()},
            repl::OpTypeEnum::kInsert,
            /*sizeDelta=*/16));
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "incompleteDelta");
    EXPECT_FALSE(results.getExpectedXxh3CollectionHash().has_value());
    EXPECT_TRUE(results.getWarnings().empty());
}

TEST_F(CollectionHashComparisonTest, ReportsIncompleteWhenAnImportedCollectionHasNoHash) {
    createFastCountContainers();
    const BSONObj doc = BSON("_id" << 1);
    insertDocs({doc});

    const auto uuid = collectionUuid();
    seedPersistedHash(uuid, 0x1234);

    // An import brings in a collection whose documents were never hashed, so its delta carries no
    // hash and nothing folded over that range can be trusted.
    replicated_fast_count::test_helpers::writeToOplog(
        operationContext(),
        replicated_fast_count::test_helpers::makeImportCollectionOplogEntry(
            Timestamp(2, 1),
            replicated_fast_count::test_helpers::NsAndUUID{kNss, uuid},
            /*numRecords=*/1,
            /*dataSize=*/doc.objsize()));
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "incompleteDelta");
    EXPECT_FALSE(results.getExpectedXxh3CollectionHash().has_value());
    EXPECT_TRUE(results.getWarnings().empty());
}

TEST_F(CollectionHashComparisonTest, AFailedComparisonDoesNotCutValidationShort) {
    createFastCountContainers();
    insertDocs({BSON("_id" << 1), BSON("_id" << 2)});
    seedPersistedHash(collectionUuid(), 0);

    // Without an oplog the replay throws. That must be reported rather than propagated: validate's
    // outer handler records a warning and skips everything after it, index validation included,
    // while still calling the collection valid.
    ASSERT_OK(
        storageInterface()->dropCollection(operationContext(), NamespaceString::kRsOplogNamespace));
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "comparisonFailed");
    EXPECT_FALSE(results.getExpectedXxh3CollectionHash().has_value());

    // The rest of validation still ran.
    ASSERT_TRUE(results.getNumRecords().has_value());
    EXPECT_EQ(*results.getNumRecords(), 2);
    EXPECT_TRUE(results.isValid());
}

TEST_F(CollectionHashComparisonTest, WarnsButStaysValidWhenThePersistedHashDisagrees) {
    createFastCountContainers();
    const std::vector<BSONObj> docs = {BSON("_id" << 1), BSON("_id" << 2)};
    insertDocs(docs);
    // Stands in for the collection having diverged from what replication believes it holds.
    seedPersistedHash(collectionUuid(), static_cast<int64_t>(xxh3Of(docs)) ^ 0xabcd);

    const auto results = hashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "mismatched");
    EXPECT_EQ(results.getWarnings().size(), 1);
    // A divergence is surfaced, but must not by itself declare the collection invalid.
    EXPECT_TRUE(results.isValid());
}

class CollectionValidationXxh3Test : public CollectionValidationTest {
protected:
    CollectionValidationXxh3Test()
        : CollectionValidationTest(Options{}.setPersistenceProvider(
              std::make_unique<ContinuousInternodeValidationProvider>())) {}

    ValidateResults collectionHashValidate(const NamespaceString& nss = kNss) {
        ValidateResults results;
        EXPECT_EQ(ErrorCodes::OK,
                  collection_validation::validate(
                      operationContext(),
                      nss,
                      collection_validation::ValidationOptions{
                          collection_validation::ValidateMode::kCollectionHash,
                          collection_validation::RepairMode::kNone,
                          /*logDiagnostics=*/false},
                      &results));
        return results;
    }

    // XORs the per-document validation hashes the same way the replicated collection validation
    // hash accumulates them.
    static uint64_t expectedXxh3(const std::vector<BSONObj>& docs) {
        uint64_t hash = 0;
        for (const auto& doc : docs) {
            hash ^= static_cast<uint64_t>(repl::computeDocValidationHash(doc));
        }
        return hash;
    }

    void insertDocs(const std::vector<BSONObj>& docs, const NamespaceString& nss = kNss) {
        const AutoGetCollection coll(operationContext(), nss, MODE_IX);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), *coll, docs));
        wuow.commit();
    }

    // Stands in for the same collection on a second node, so the two hashes can be compared the
    // way continuous internode validation compares them across a replica set.
    const NamespaceString& secondNss() {
        static const NamespaceString nss =
            NamespaceString::createNamespaceString_forTest("test.t2");
        return nss;
    }

    void createSecondCollection(const std::vector<BSONObj>& docs) {
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), secondNss(), CollectionOptions{}));
        insertDocs(docs, secondNss());
    }
};

TEST_F(CollectionValidationXxh3Test, ReportsWhyNoComparisonHappened) {
    // An untracked replicated hash doesn't get compared.
    const auto results = collectionHashValidate();
    ASSERT_TRUE(results.getHashComparison().has_value());
    EXPECT_EQ(toString(*results.getHashComparison()), "notTracked");
    EXPECT_FALSE(results.getExpectedXxh3CollectionHash().has_value());
}

TEST_F(CollectionValidationXxh3Test, EmptyCollectionHashesToZero) {
    const auto results = collectionHashValidate();
    ASSERT_TRUE(results.getXxh3CollectionHash().has_value());
    EXPECT_EQ(*results.getXxh3CollectionHash(), 0U);
}

TEST_F(CollectionValidationXxh3Test, HashIsTheXorOfThePerDocumentHashes) {
    const std::vector<BSONObj> docs = {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)};
    insertDocs(docs);

    const auto results = collectionHashValidate();
    ASSERT_TRUE(results.getXxh3CollectionHash().has_value());
    EXPECT_EQ(*results.getXxh3CollectionHash(), expectedXxh3(docs));
}

TEST_F(CollectionValidationXxh3Test, SameContentsInAnyOrderHashTheSame) {
    insertDocs({BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)});
    createSecondCollection({BSON("_id" << 3), BSON("_id" << 1), BSON("_id" << 2)});

    const auto first = collectionHashValidate();
    const auto second = collectionHashValidate(secondNss());
    ASSERT_TRUE(first.getXxh3CollectionHash().has_value());
    ASSERT_TRUE(second.getXxh3CollectionHash().has_value());
    EXPECT_EQ(*first.getXxh3CollectionHash(), *second.getXxh3CollectionHash());
}

TEST_F(CollectionValidationXxh3Test, DifferingContentsHashDifferently) {
    insertDocs({BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)});
    createSecondCollection({BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 4)});

    const auto first = collectionHashValidate();
    const auto second = collectionHashValidate(secondNss());
    ASSERT_TRUE(first.getXxh3CollectionHash().has_value());
    ASSERT_TRUE(second.getXxh3CollectionHash().has_value());
    EXPECT_NE(*first.getXxh3CollectionHash(), *second.getXxh3CollectionHash());
}

TEST_F(CollectionValidationXxh3Test, MissingDocumentHashesDifferently) {
    insertDocs({BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)});
    createSecondCollection({BSON("_id" << 1), BSON("_id" << 2)});

    const auto first = collectionHashValidate();
    const auto second = collectionHashValidate(secondNss());
    ASSERT_TRUE(first.getXxh3CollectionHash().has_value());
    ASSERT_TRUE(second.getXxh3CollectionHash().has_value());
    EXPECT_NE(*first.getXxh3CollectionHash(), *second.getXxh3CollectionHash());
}

TEST_F(CollectionValidationXxh3Test, RecordThatIsNotValidBSONIsStillHashed) {
    const auto doc = BSON("_id" << 1);
    insertDocs({doc});
    ASSERT_EQ(1, setUpInvalidData(operationContext()));

    const auto results = collectionHashValidate();
    EXPECT_FALSE(results.isValid());
    ASSERT_TRUE(results.getXxh3CollectionHash().has_value());

    // The corrupt record contributes its raw bytes, which is what the accompanying warning is
    // about.
    const auto invalidBson = "\0\0\0\0\0"sv;
    EXPECT_EQ(*results.getXxh3CollectionHash(),
              static_cast<uint64_t>(repl::computeDocValidationHash(doc)) ^
                  static_cast<uint64_t>(repl::computeDocValidationHash(
                      ConstDataRange(invalidBson.data(), invalidBson.size()))));
}

TEST_F(CollectionValidationTest, NoXxh3HashWithoutContinuousInternodeValidation) {
    {
        const AutoGetCollection coll(operationContext(), kNss, MODE_IX);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), *coll, BSON("_id" << 1)));
        wuow.commit();
    }

    ValidateResults results;
    EXPECT_EQ(
        ErrorCodes::OK,
        collection_validation::validate(operationContext(),
                                        kNss,
                                        collection_validation::ValidationOptions{
                                            collection_validation::ValidateMode::kCollectionHash,
                                            collection_validation::RepairMode::kNone,
                                            /*logDiagnostics=*/false},
                                        &results));

    // The default attached storage provider does not record the per-document validation hashes,
    // so there is nothing for this hash to be compared against.
    ASSERT_TRUE(results.getCollectionHash().has_value());
    EXPECT_FALSE(results.getXxh3CollectionHash().has_value());

    // The comparison does not apply to a storage model that carries no per-document validation
    // hashes, so no outcome is reported at all.
    EXPECT_FALSE(results.getHashComparison().has_value());
}

}  // namespace
}  // namespace mongo

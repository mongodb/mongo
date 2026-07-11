// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/key_string_index_consistency.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/validate_gen.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/progress_meter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
using namespace std::literals::string_view_literals;
namespace {

// Namespace String for the collection used in these tests.
const auto kNss = NamespaceString::createNamespaceString_forTest(
    "keyStringIndexConsistencyDB.keyStringIndexConsistencyColl"sv);
const auto kDefaultValidateOptions =
    collection_validation::ValidationOptions{collection_validation::ValidateMode::kForegroundFull,
                                             collection_validation::RepairMode::kNone,
                                             /*logDiagnostics=*/true};

using KeyStringIndexConsistencyTest = CatalogTestFixture;


ValidateResults validate(OperationContext* opCtx) {
    ValidateResults validateResults;
    ASSERT_OK(
        collection_validation::validate(opCtx, kNss, kDefaultValidateOptions, &validateResults));
    return validateResults;
}

// Clears the collection without updating indexes, this creates extra index entries.
void clearCollection(OperationContext* opCtx, const CollectionPtr& coll) {
    RecordStore* rs = coll->getRecordStore();
    ASSERT_OK(rs->truncate(opCtx, *shard_role_details::getRecoveryUnit(opCtx)));
}

// Removes entries found in the given collection from this index, this creates missing index
// entries.
void clearIndexOfEntriesFoundInCollection(OperationContext* opCtx,
                                          CollectionWriter& coll,
                                          const IndexCatalogEntry* entry) {
    auto iam = entry->accessMethod()->asSortedData();
    auto cursor = coll->getCursor(opCtx);
    for (auto record = cursor->next(); record; record = cursor->next()) {
        SharedBufferFragmentBuilder pooledBuilder(
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
        int64_t numDeleted = 0;
        KeyStringSet keys;
        iam->getKeys(opCtx,
                     coll.get(),
                     entry,
                     pooledBuilder,
                     record->data.toBson(),
                     InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
                     SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                     &keys,
                     nullptr,
                     nullptr,
                     record->id);
        ASSERT_OK(iam->removeKeys(opCtx,
                                  *shard_role_details::getRecoveryUnit(opCtx),
                                  coll.get(),
                                  entry,
                                  keys,
                                  InsertDeleteOptions{.dupsAllowed = true},
                                  &numDeleted));
        ASSERT_EQ(1, numDeleted);
    }
}

}  // namespace

TEST_F(KeyStringIndexConsistencyTest, ExtraIndexEntriesLimitedByMemoryBounds) {

    // Setup a collection with extra index entries.
    {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
        AutoGetCollection coll(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());

        for (int i = 0; i < 10; ++i) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(Helpers::insert(operationContext(), *coll, doc));
        }

        clearCollection(operationContext(), *coll);
        wuow.commit();
    }

    // With the default memory limiters, all inconsistencies will be found.
    {
        ValidateResults results = validate(operationContext());
        ASSERT_EQ(results.getExtraIndexEntries().size(), 10);
    }

    // With less memory permitted, fewer inconsistencies will be found.
    {
        ON_BLOCK_EXIT([]() { maxValidateMemoryUsageMB.store(kMaxValidateMemoryUsageMBDefault); });
        maxValidateMemoryUsageMB.store(0);

        ValidateResults results = validate(operationContext());
        ASSERT_LT(results.getExtraIndexEntries().size(), 10);
        // 1 bucket is always kept, so even with 0 memory we will at least find one inconsistency.
        ASSERT_GTE(results.getExtraIndexEntries().size(), 1);
    }
}

TEST_F(KeyStringIndexConsistencyTest, MissingIndexEntriesLimitedByMemoryBounds) {
    // Setup a collection with multiple missing index entries.
    {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
        AutoGetCollection coll(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), coll};

        for (int i = 0; i < 10; ++i) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(Helpers::insert(operationContext(), writer.get(), doc));
        }

        IndexCatalog* indexCatalog =
            writer.getWritableCollection(operationContext())->getIndexCatalog();
        clearIndexOfEntriesFoundInCollection(
            operationContext(), writer, indexCatalog->findIdIndex(operationContext()));
        wuow.commit();
    }


    // With the default memory limiters, all inconsistencies will be found.
    {
        ValidateResults results = validate(operationContext());
        ASSERT_EQ(results.getMissingIndexEntries().size(), 10);
    }

    // With less memory permitted, fewer inconsistencies will be found.
    {
        ON_BLOCK_EXIT([]() { maxValidateMemoryUsageMB.store(kMaxValidateMemoryUsageMBDefault); });
        maxValidateMemoryUsageMB.store(0);

        ValidateResults results = validate(operationContext());
        ASSERT_LT(results.getMissingIndexEntries().size(), 10);
        // 1 bucket is always kept, so even with 0 memory we will at least find one inconsistency.
        ASSERT_GTE(results.getMissingIndexEntries().size(), 1);
    }
}

TEST_F(KeyStringIndexConsistencyTest, ExtraEntryPartialFindingsWithNonzeroMemoryLimit) {
    // This collection has an index where the keys are about 600KB each.
    {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
        AutoGetCollection coll(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), coll};

        IndexSpec spec;
        auto collWriter = writer.getWritableCollection(operationContext());
        ASSERT_OK(collWriter->getIndexCatalog()->createIndexOnEmptyCollection(
            operationContext(),
            collWriter,
            BSON("name" << "a_1"
                        << "v" << int(IndexConfig::kLatestIndexVersion) << "key"
                        << BSON("a" << 1))));
        for (int i = 0; i < 10; ++i) {
            BSONObj doc = BSON("_id" << i << "a" << std::string(600 * 1024, 'a' + i));
            ASSERT_OK(Helpers::insert(operationContext(), writer.get(), doc));
        }

        clearCollection(operationContext(), writer.get());
        wuow.commit();
    }

    // Due to the very large keystrings, the number of reported entries will be smaller than the
    // real number. But we can still parse the real number out of a particular warning.
    auto getRealExtraEntryCount = [](const ValidateResults& results) {
        const std::string& firstWarning = *results.getWarnings().begin();
        // It's "Detected XX extra index entries.", so get the number between the first two spaces.
        auto firstSpace = firstWarning.find(' ');
        return std::stoi(firstWarning.substr(firstSpace, firstWarning.find(firstSpace + 1)));
    };

    {
        // Do a validation without limits, we expect 10 from each of the "_id" and "a" indexes.
        ValidateResults results = validate(operationContext());
        ASSERT_EQ(getRealExtraEntryCount(results), 20);
    }

    {
        ON_BLOCK_EXIT([]() { maxValidateMemoryUsageMB.store(kMaxValidateMemoryUsageMBDefault); });
        maxValidateMemoryUsageMB.store(2);
        ValidateResults results = validate(operationContext());
        // With a 2MB limit we'll at most keep all _id buckets (~5B each) and 3 a_1 buckets (~600KB
        // each), so no more than 13.
        ASSERT_LTE(getRealExtraEntryCount(results), 13);
        // For the purpose of this test, just check that we do find most of the above (given that
        // the hash is deterministic, we'll actually find all of them).
        ASSERT_GTE(getRealExtraEntryCount(results), 8);
    }
}

TEST_F(KeyStringIndexConsistencyTest, MissingEntryPartialFindingsWithNonzeroMemoryLimit) {
    // This collection has an index where the keys are about 600KB each.
    {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
        AutoGetCollection coll(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), coll};

        IndexSpec spec;
        auto writeColl = writer.getWritableCollection(operationContext());
        ASSERT_OK(writeColl->getIndexCatalog()->createIndexOnEmptyCollection(
            operationContext(),
            writeColl,
            BSON("name" << "a_1"
                        << "v" << int(IndexConfig::kLatestIndexVersion) << "key"
                        << BSON("a" << 1))));
        for (int i = 0; i < 10; ++i) {
            BSONObj doc = BSON("_id" << i << "a" << std::string(600 * 1024, 'a' + i));
            ASSERT_OK(Helpers::insert(operationContext(), writer.get(), doc));
        }

        IndexCatalog* indexCatalog =
            writer.getWritableCollection(operationContext())->getIndexCatalog();
        clearIndexOfEntriesFoundInCollection(
            operationContext(), writer, indexCatalog->findIndexByName(operationContext(), "a_1"));
        wuow.commit();
    }

    // Due to the very large keystrings, the number of reported entries will be smaller than the
    // real number. But we can still parse the real number out of a particular warning.
    auto getRealMissingEntryCount = [](const ValidateResults& results) {
        const std::string& firstWarning = *results.getWarnings().begin();
        // It's "Detected XX missing index entries.", so get the number between the first two
        // spaces.
        auto firstSpace = firstWarning.find(' ');
        return std::stoi(firstWarning.substr(firstSpace, firstWarning.find(firstSpace + 1)));
    };

    {
        // Do a validation without limits, 10 entries were removed from a_1.
        ValidateResults results = validate(operationContext());
        ASSERT_EQ(getRealMissingEntryCount(results), 10);
    }

    {
        ON_BLOCK_EXIT([]() { maxValidateMemoryUsageMB.store(kMaxValidateMemoryUsageMBDefault); });
        maxValidateMemoryUsageMB.store(2);
        ValidateResults results = validate(operationContext());
        // With a 2MB limit we'll at most keep 3 a_1 buckets (~600KB each).
        ASSERT_LTE(getRealMissingEntryCount(results), 3);
        // For the purpose of this test, just check that we do find some of the above (given that
        // the hash is deterministic, we'll actually find all of them).
        ASSERT_GTE(getRealMissingEntryCount(results), 1);
    }
}

TEST_F(KeyStringIndexConsistencyTest, MemoryLimitSharedBetweenMissingAndExtra) {

    // Setup a collection with extra and missing index entries.
    {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
        AutoGetCollection coll(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), coll};

        // The first 10 entries appear in the index and not the collection.
        for (int i = 0; i < 10; ++i) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(Helpers::insert(operationContext(), writer.get(), doc));
        }
        clearCollection(operationContext(), writer.get());

        // The second 10 entries are collection-only.
        for (int i = 10; i < 20; ++i) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(Helpers::insert(operationContext(), writer.get(), doc));
        }
        IndexCatalog* indexCatalog =
            writer.getWritableCollection(operationContext())->getIndexCatalog();
        // Because we already cleared the collection of entries [0-10[, this call means the index
        // retains those.
        clearIndexOfEntriesFoundInCollection(
            operationContext(), writer, indexCatalog->findIdIndex(operationContext()));

        wuow.commit();
    }

    // With the default memory limiters, all inconsistencies will be found.
    {
        ValidateResults results = validate(operationContext());
        ASSERT_EQ(results.getExtraIndexEntries().size(), 10);
        ASSERT_EQ(results.getMissingIndexEntries().size(), 10);
    }

    // With less memory permitted, fewer inconsistencies will be found.
    {
        ON_BLOCK_EXIT([]() { maxValidateMemoryUsageMB.store(kMaxValidateMemoryUsageMBDefault); });
        maxValidateMemoryUsageMB.store(0);

        ValidateResults results = validate(operationContext());
        // The memory budget is shared between missing/extra, so limiting it will limit the total
        // number of findings (when we could find both).
        size_t sum_of_inconsistencies =
            results.getExtraIndexEntries().size() + results.getMissingIndexEntries().size();
        ASSERT_LT(sum_of_inconsistencies, 20);
        // 1 bucket is always kept, so even with 0 memory we will at least find one inconsistency.
        ASSERT_GTE(sum_of_inconsistencies, 1);
    }
}

// Index key consistency is validated by rebuilding the key for a given document in a collection.
// This test exercises the failure path by creating a hashed index which is incompatible with
// array-type BSON data. This document will then be subject to key consistency checks that it will
// fail.
TEST_F(KeyStringIndexConsistencyTest, FailedKeygen) {
    auto opCtx = operationContext();
    ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions()));

    static constexpr auto secondaryIndexKey{"xHashed"sv};

    AutoGetCollection coll(opCtx, kNss, MODE_X);
    CollectionWriter writer(opCtx, coll);
    const auto indexSpec = BSON("v" << IndexDescriptor::IndexVersion::kV2 << "name"
                                    << secondaryIndexKey << "key" << BSON("x" << "hashed"));
    {
        WriteUnitOfWork wuow(opCtx);
        auto collWriter = writer.getWritableCollection(opCtx);
        ASSERT_OK(collWriter->getIndexCatalog()->createIndexOnEmptyCollection(
            opCtx, collWriter, indexSpec));

        ASSERT_OK(Helpers::insert(opCtx, writer.get(), BSON("_id" << 1 << "x" << "y")));
        wuow.commit();
    }

    collection_validation::ValidateState state(opCtx, kNss, kDefaultValidateOptions);

    const auto unhashableDoc = std::invoke([] {
        BSONArrayBuilder bab;
        bab.append(1);
        return BSON("x" << bab.arr());
    });

    const auto xHashedIndex = coll->getIndexCatalog()->findIndexByName(opCtx, secondaryIndexKey);

    ValidateResults results;
    KeyStringIndexConsistency ksic(opCtx, &state);
    ksic.traverseRecord(opCtx, *coll, xHashedIndex, RecordId(1), unhashableDoc, &results);
    const auto& errors = results.getErrors();
    namespace m = unittest::match;
    ASSERT_THAT(errors, m::ElementsAre(m::HasSubstr("16766")))
        << "Expected to find error 16766 relating to hashed indexes unsupported on array types";
    auto it = errors.begin();
    LOGV2(11475700, "Error associated with validation run", "validationError"_attr = *it);
}

TEST_F(KeyStringIndexConsistencyTest, GeoKeygenFailureReportsStructuredError) {
    // A real out-of-bounds GeoJSON point makes 2dsphere key generation throw
    // GeoKeyExtractionFailureInfo. Validate reports a structured error naming the index, path, and
    // code (not the per-document reason or _id, which stay in the log), and keeps validating.
    auto opCtx = operationContext();
    ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions()));

    static constexpr auto geoIndexName{"loc_2dsphere"sv};

    AutoGetCollection coll(opCtx, kNss, MODE_X);
    CollectionWriter writer(opCtx, coll);
    const auto indexSpec =
        BSON("v" << IndexDescriptor::IndexVersion::kV2 << "name" << geoIndexName << "key"
                 << BSON("loc" << "2dsphere") << "2dsphereIndexVersion" << 3);
    {
        WriteUnitOfWork wuow(opCtx);
        auto collWriter = writer.getWritableCollection(opCtx);
        ASSERT_OK(collWriter->getIndexCatalog()->createIndexOnEmptyCollection(
            opCtx, collWriter, indexSpec));
        ASSERT_OK(Helpers::insert(
            opCtx, writer.get(), fromjson("{_id: 1, loc: {type: 'Point', coordinates: [0, 0]}}")));
        wuow.commit();
    }

    collection_validation::ValidateState state(opCtx, kNss, kDefaultValidateOptions);

    // Longitude 200 is out of bounds. We assert that neither the _id ('secret-id') nor the
    // per-document reason ('out of bounds') reaches res.errors; both stay in the log.
    const auto badGeoDoc =
        fromjson("{_id: 'secret-id', loc: {type: 'Point', coordinates: [200, 0]}}");
    const auto geoIndex = coll->getIndexCatalog()->findIndexByName(opCtx, geoIndexName);

    ValidateResults results;
    KeyStringIndexConsistency ksic(opCtx, &state);
    ksic.traverseRecord(opCtx, *coll, geoIndex, RecordId(1), badGeoDoc, &results);

    using testing::HasSubstr;
    using testing::Not;

    const auto& errors = results.getErrors();
    ASSERT_EQ(errors.size(), 1);
    const auto& error = *errors.begin();
    EXPECT_THAT(error, HasSubstr("at path loc"));
    EXPECT_THAT(error, HasSubstr("BadValue"));
    EXPECT_THAT(error, Not(HasSubstr("out of bounds")));
    EXPECT_THAT(error, Not(HasSubstr("secret-id")));
    EXPECT_THAT(error, HasSubstr("12565600"));
    EXPECT_TRUE(results.continueValidation());
}

TEST_F(KeyStringIndexConsistencyTest, GeoKeygenFailuresCollapseAcrossDocuments) {
    // res.errors keys on (index, path, code) with no per-document content, so documents failing on
    // the same index and path collapse to a single error regardless of their values. This keeps
    // res.errors bounded by the schema rather than by the number of bad documents.
    auto opCtx = operationContext();
    ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions()));

    static constexpr auto geoIndexName{"loc_2dsphere"sv};

    AutoGetCollection coll(opCtx, kNss, MODE_X);
    CollectionWriter writer(opCtx, coll);
    const auto indexSpec =
        BSON("v" << IndexDescriptor::IndexVersion::kV2 << "name" << geoIndexName << "key"
                 << BSON("loc" << "2dsphere") << "2dsphereIndexVersion" << 3);
    {
        WriteUnitOfWork wuow(opCtx);
        auto collWriter = writer.getWritableCollection(opCtx);
        ASSERT_OK(collWriter->getIndexCatalog()->createIndexOnEmptyCollection(
            opCtx, collWriter, indexSpec));
        ASSERT_OK(Helpers::insert(
            opCtx, writer.get(), fromjson("{_id: 1, loc: {type: 'Point', coordinates: [0, 0]}}")));
        wuow.commit();
    }

    collection_validation::ValidateState state(opCtx, kNss, kDefaultValidateOptions);
    const auto geoIndex = coll->getIndexCatalog()->findIndexByName(opCtx, geoIndexName);

    ValidateResults results;
    KeyStringIndexConsistency ksic(opCtx, &state);

    // Four documents at distinct out-of-bounds longitudes, all on the same index and path.
    const int badLongitudes[] = {200, 201, 202, 203};
    int64_t recordId = 1;
    for (int lng : badLongitudes) {
        const auto doc =
            BSON("loc" << BSON("type" << "Point" << "coordinates" << BSON_ARRAY(lng << 0)));
        ksic.traverseRecord(opCtx, *coll, geoIndex, RecordId(recordId++), doc, &results);
    }

    using testing::HasSubstr;

    const auto& errors = results.getErrors();
    ASSERT_EQ(errors.size(), 1);
    EXPECT_THAT(*errors.begin(), HasSubstr("at path loc"));
}

// Splitting the record-store scan into disjoint slices and merging the per-slice
// KeyStringIndexConsistency objects must reproduce a single serial scan, regardless of merge order.
// This is the property that lets the scan be parallelized: the first-phase bucket counts are an
// order-independent additive hash.
TEST_F(KeyStringIndexConsistencyTest, MergeIsCommutativeAndMatchesSerialScan) {
    auto opCtx = operationContext();
    ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions()));

    // A secondary index plus a mix of scalar and array values spreads keys non-trivially across
    // buckets; the array values make 'a_1' multikey, which also exercises the multikey portions of
    // the merge.
    {
        AutoGetCollection autoColl(opCtx, kNss, MODE_X);
        CollectionWriter writer(opCtx, autoColl);
        WriteUnitOfWork wuow(opCtx);
        auto collWriter = writer.getWritableCollection(opCtx);
        ASSERT_OK(collWriter->getIndexCatalog()->createIndexOnEmptyCollection(
            opCtx,
            collWriter,
            BSON("name" << "a_1"
                        << "v" << int(IndexConfig::kLatestIndexVersion) << "key"
                        << BSON("a" << 1))));
        for (int i = 0; i < 12; ++i) {
            BSONObj doc = (i % 4 == 0) ? BSON("_id" << i << "a" << BSON_ARRAY(i << (i + 100)))
                                       : BSON("_id" << i << "a" << i);
            ASSERT_OK(Helpers::insert(opCtx, writer.get(), doc));
        }
        wuow.commit();
    }

    AutoGetCollection coll(opCtx, kNss, MODE_X);
    collection_validation::ValidateState state(opCtx, kNss, kDefaultValidateOptions);
    ASSERT_OK(state.initializeCollection(opCtx));

    // Snapshot (RecordId, document) pairs up front so disjoint slices can be replayed into separate
    // consistency objects. Phase-one traverseRecord() reads keys via the index access method and
    // does not touch ValidateState's cursors, so a single shared ValidateState is sufficient.
    std::vector<std::pair<RecordId, BSONObj>> records;
    {
        auto cursor = coll->getCursor(opCtx);
        for (auto rec = cursor->next(); rec; rec = cursor->next()) {
            records.emplace_back(rec->id, rec->data.toBson().getOwned());
        }
    }
    ASSERT_GTE(records.size(), 4u);

    // Runs the first-phase document scan over records [begin, end) for every index.
    auto scanSlice = [&](KeyStringIndexConsistency& ksic, size_t begin, size_t end) {
        ValidateResults results;
        for (size_t i = begin; i < end; ++i) {
            for (const auto& indexIdent : state.getIndexIdents()) {
                const auto* entry = coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
                ksic.traverseRecord(
                    opCtx, *coll, entry, records[i].first, records[i].second, &results);
            }
        }
        EXPECT_TRUE(results.isValid()) << "first-phase scan unexpectedly reported errors";
    };

    // A comparable fingerprint of the merge-relevant bucket state.
    auto bucketsOf = [](const KeyStringIndexConsistency& ksic) {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (const auto& bucket : ksic.getIndexKeyBuckets()) {
            out.emplace_back(bucket.indexKeyCount, bucket.bucketSizeBytes);
        }
        return out;
    };

    const size_t mid = records.size() / 2;

    // Serial baseline: a single object scans every record.
    KeyStringIndexConsistency serial(opCtx, &state);
    scanSlice(serial, 0, records.size());

    // Order A->B: slice one merges in slice two.
    KeyStringIndexConsistency ab(opCtx, &state);
    {
        KeyStringIndexConsistency second(opCtx, &state);
        scanSlice(ab, 0, mid);
        scanSlice(second, mid, records.size());
        ab.merge(second);
    }

    // Order B->A: slice two merges in slice one. Fresh objects, since merge() consumes its
    // argument.
    KeyStringIndexConsistency ba(opCtx, &state);
    {
        KeyStringIndexConsistency first(opCtx, &state);
        scanSlice(ba, mid, records.size());
        scanSlice(first, 0, mid);
        ba.merge(first);
    }

    // Both merge orders reproduce the serial scan exactly...
    EXPECT_TRUE(bucketsOf(ab) == bucketsOf(serial));
    EXPECT_TRUE(bucketsOf(ba) == bucketsOf(serial));
    EXPECT_EQ(ab.getTotalIndexKeys(), serial.getTotalIndexKeys());
    EXPECT_EQ(ba.getTotalIndexKeys(), serial.getTotalIndexKeys());

    // ...and therefore agree with each other (commutativity).
    EXPECT_TRUE(bucketsOf(ab) == bucketsOf(ba));
    EXPECT_EQ(ab.getTotalIndexKeys(), ba.getTotalIndexKeys());
}

// Drives a full two-phase index consistency scan over the given collection, leaving the returned
// object in the second phase with its '_missingIndexEntries'/'_extraIndexEntries' maps populated.
KeyStringIndexConsistency runTwoPhaseScan(OperationContext* opCtx,
                                          collection_validation::ValidateState& state,
                                          const CollectionPtr& coll) {
    // addIndexEntryErrors() looks up each inconsistent index by name in the results map, so the map
    // must be pre-populated the same way _validateIndexes() does in the real validation flow.
    auto prepopulateIndexResults = [&](ValidateResults& results) {
        for (const auto& indexIdent : state.getIndexIdents()) {
            const auto* entry = coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
            results.getIndexValidateResult(entry->descriptor()->indexName());
        }
    };

    // traverseIndex() only touches the progress meter while iterating real index entries, but a
    // live meter is still required for collections that have extra index entries to report.
    ProgressMeterHolder progress;
    {
        std::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(lk, CurOp::get(opCtx)->setProgress(lk, "test validate", 1), opCtx);
    }

    KeyStringIndexConsistency ksic(opCtx, &state);

    // Scans every record (incrementing buckets) and then every index entry (decrementing buckets).
    // Buckets left non-zero after both scans indicate inconsistencies.
    auto scanAll = [&](ValidateResults& results) {
        auto cursor = coll->getCursor(opCtx);
        for (auto rec = cursor->next(); rec; rec = cursor->next()) {
            for (const auto& indexIdent : state.getIndexIdents()) {
                const auto* entry = coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
                ksic.traverseRecord(
                    opCtx, coll, entry, rec->id, rec->data.toBson().getOwned(), &results);
            }
        }
        for (const auto& indexIdent : state.getIndexIdents()) {
            const auto* entry = coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
            ksic.traverseIndex(opCtx, entry, progress, &results);
        }
    };

    ValidateResults phaseOneResults;
    prepopulateIndexResults(phaseOneResults);
    scanAll(phaseOneResults);

    ksic.setSecondPhase();
    ksic.limitMemoryUsageForSecondPhase(&phaseOneResults);

    // The second-phase scan records the specific keys that hashed to inconsistent buckets into the
    // '_missingIndexEntries'/'_extraIndexEntries' maps.
    ValidateResults phaseTwoResults;
    prepopulateIndexResults(phaseTwoResults);
    scanAll(phaseTwoResults);

    return ksic;
}

// Builds a ValidateResults whose per-index results map is populated, so addIndexEntryErrors() can
// look each inconsistent index up by name.
ValidateResults makeResultsWithIndexMap(OperationContext* opCtx,
                                        collection_validation::ValidateState& state,
                                        const CollectionPtr& coll) {
    ValidateResults results;
    for (const auto& indexIdent : state.getIndexIdents()) {
        const auto* entry = coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
        results.getIndexValidateResult(entry->descriptor()->indexName());
    }
    return results;
}

// Sets up a collection with both extra and missing _id index entries, leaving 5 of each.
void setupCollectionWithInconsistencies(OperationContext* opCtx, repl::StorageInterface* storage) {
    ASSERT_OK(storage->createCollection(opCtx, kNss, CollectionOptions()));
    AutoGetCollection coll(opCtx, kNss, MODE_X);
    WriteUnitOfWork wuow(opCtx);
    CollectionWriter writer{opCtx, coll};

    // The first 5 entries appear in the index but not the collection (extra index entries).
    for (int i = 0; i < 5; ++i) {
        ASSERT_OK(Helpers::insert(opCtx, writer.get(), BSON("_id" << i)));
    }
    clearCollection(opCtx, writer.get());

    // The second 5 entries appear in the collection but not the index (missing index entries).
    for (int i = 5; i < 10; ++i) {
        ASSERT_OK(Helpers::insert(opCtx, writer.get(), BSON("_id" << i)));
    }
    IndexCatalog* indexCatalog = writer.getWritableCollection(opCtx)->getIndexCatalog();
    clearIndexOfEntriesFoundInCollection(opCtx, writer, indexCatalog->findIdIndex(opCtx));

    wuow.commit();
}

// Asserts the two index-entry reports are element-for-element identical. The reports come from
// std::maps ordered by (indexName, KeyString), so a faithful copy must reproduce the same entries
// in the same order.
void assertEntriesMatch(const std::vector<BSONObj>& expected, const std::vector<BSONObj>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_BSONOBJ_EQ(expected[i], actual[i]);
    }
}

TEST_F(KeyStringIndexConsistencyTest, CopyConstructorPreservesSecondPhaseInconsistencies) {
    auto opCtx = operationContext();
    setupCollectionWithInconsistencies(opCtx, storageInterface());

    AutoGetCollection coll(opCtx, kNss, MODE_X);
    collection_validation::ValidateState state(opCtx, kNss, kDefaultValidateOptions);
    ASSERT_OK(state.initializeCollection(opCtx));
    state.initializeCursors(opCtx);

    KeyStringIndexConsistency original = runTwoPhaseScan(opCtx, state, *coll);
    KeyStringIndexConsistency copy(original);

    // Gathering errors dereferences the IndexInfo* pointers held by the inconsistency maps; on the
    // copy this must not crash and must reproduce the original's reported entries exactly.
    ValidateResults originalResults = makeResultsWithIndexMap(opCtx, state, *coll);
    ValidateResults copyResults = makeResultsWithIndexMap(opCtx, state, *coll);
    original.addIndexEntryErrors(opCtx, &originalResults);
    copy.addIndexEntryErrors(opCtx, &copyResults);

    // Sanity check that the scan actually produced inconsistencies, so the comparison is
    // meaningful.
    ASSERT_EQ(originalResults.getMissingIndexEntries().size(), 5);
    ASSERT_EQ(originalResults.getExtraIndexEntries().size(), 5);

    assertEntriesMatch(originalResults.getMissingIndexEntries(),
                       copyResults.getMissingIndexEntries());
    assertEntriesMatch(originalResults.getExtraIndexEntries(), copyResults.getExtraIndexEntries());
}

TEST_F(KeyStringIndexConsistencyTest, CopyAssignmentPreservesSecondPhaseInconsistencies) {
    auto opCtx = operationContext();
    setupCollectionWithInconsistencies(opCtx, storageInterface());

    AutoGetCollection coll(opCtx, kNss, MODE_X);
    collection_validation::ValidateState state(opCtx, kNss, kDefaultValidateOptions);
    ASSERT_OK(state.initializeCollection(opCtx));
    state.initializeCursors(opCtx);

    KeyStringIndexConsistency original = runTwoPhaseScan(opCtx, state, *coll);

    // Assign into a fresh, first-phase object to exercise the copy assignment path.
    KeyStringIndexConsistency assigned(opCtx, &state);
    assigned = original;

    ValidateResults originalResults = makeResultsWithIndexMap(opCtx, state, *coll);
    ValidateResults assignedResults = makeResultsWithIndexMap(opCtx, state, *coll);
    original.addIndexEntryErrors(opCtx, &originalResults);
    assigned.addIndexEntryErrors(opCtx, &assignedResults);

    // Sanity check that the scan actually produced inconsistencies, so the comparison is
    // meaningful.
    ASSERT_EQ(originalResults.getMissingIndexEntries().size(), 5);
    ASSERT_EQ(originalResults.getExtraIndexEntries().size(), 5);

    assertEntriesMatch(originalResults.getMissingIndexEntries(),
                       assignedResults.getMissingIndexEntries());
    assertEntriesMatch(originalResults.getExtraIndexEntries(),
                       assignedResults.getExtraIndexEntries());
}

}  // namespace mongo

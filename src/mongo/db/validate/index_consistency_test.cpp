/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/validate/index_consistency.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/validate_gen.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Namespace String for the collection used in these tests.
const auto kNss =
    NamespaceString::createNamespaceString_forTest("indexConsistencyDB.indexConsistencyColl");

using IndexConsistencyTest = CatalogTestFixture;


ValidateResults validate(OperationContext* opCtx) {
    ValidateResults validateResults;
    ASSERT_OK(CollectionValidation::validate(
        opCtx,
        kNss,
        CollectionValidation::ValidationOptions{CollectionValidation::ValidateMode::kForegroundFull,
                                                CollectionValidation::RepairMode::kNone,
                                                /*logDiagnostics=*/true},
        &validateResults));
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
                                          const IndexDescriptor* descriptor) {
    IndexCatalog* indexCatalog = coll.getWritableCollection(opCtx)->getIndexCatalog();
    auto iam = indexCatalog->getEntry(descriptor)->accessMethod()->asSortedData();
    auto cursor = coll->getCursor(opCtx);
    for (auto record = cursor->next(); record; record = cursor->next()) {
        SharedBufferFragmentBuilder pooledBuilder(
            key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
        int64_t numDeleted = 0;
        KeyStringSet keys;
        iam->getKeys(opCtx,
                     coll.get(),
                     descriptor->getEntry(),
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
                                  descriptor->getEntry(),
                                  std::move(keys),
                                  InsertDeleteOptions{.dupsAllowed = true},
                                  &numDeleted));
        ASSERT_EQ(1, numDeleted);
    }
}

}  // namespace

TEST_F(IndexConsistencyTest, ExtraIndexEntriesLimitedByMemoryBounds) {

    // Setup a collection with extra index entries.
    {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
        AutoGetCollection coll(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());

        for (int i = 0; i < 10; ++i) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), *coll, InsertStatement(doc), nullptr));
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

TEST_F(IndexConsistencyTest, MissingIndexEntriesLimitedByMemoryBounds) {
    // Setup a collection with multiple missing index entries.
    {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
        AutoGetCollection coll(operationContext(), kNss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), coll};

        for (int i = 0; i < 10; ++i) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), writer.get(), InsertStatement(doc), nullptr));
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

TEST_F(IndexConsistencyTest, ExtraEntryPartialFindingsWithNonzeroMemoryLimit) {
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
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), writer.get(), InsertStatement(doc), nullptr));
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

TEST_F(IndexConsistencyTest, MissingEntryPartialFindingsWithNonzeroMemoryLimit) {
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
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), writer.get(), InsertStatement(doc), nullptr));
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

TEST_F(IndexConsistencyTest, MemoryLimitSharedBetweenMissingAndExtra) {

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
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), writer.get(), InsertStatement(doc), nullptr));
        }
        clearCollection(operationContext(), writer.get());

        // The second 10 entries are collection-only.
        for (int i = 10; i < 20; ++i) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(collection_internal::insertDocument(
                operationContext(), writer.get(), InsertStatement(doc), nullptr));
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

}  // namespace mongo

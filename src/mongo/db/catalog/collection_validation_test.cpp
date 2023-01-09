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

#include <string>

#include "mongo/bson/util/builder.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/column_index_consistency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/column_key_generator.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/idl/server_parameter_test_util.h"
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
        const CollectionOptions defaultCollectionOptions;
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
 * Calls validate on collection nss with both kValidateFull and kValidateNormal validation levels
 * and verifies the results.
 *
 * Returns the list of validation results.
 */
std::vector<std::pair<BSONObj, ValidateResults>> foregroundValidate(
    const NamespaceString& nss,
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
            opCtx, nss, mode, repairMode, &validateResults, &output));
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
 * Calls validate on collection nss with {background:true} and verifies the results.
 * If 'runForegroundAsWell' is set, then foregroundValidate() above will be run in addition.
 *
 * Returns the list of validation results.
 */
std::vector<std::pair<BSONObj, ValidateResults>> backgroundValidate(const NamespaceString& nss,
                                                                    OperationContext* opCtx,
                                                                    bool valid,
                                                                    int numRecords,
                                                                    int numInvalidDocuments,
                                                                    int numErrors,
                                                                    bool runForegroundAsWell) {
    std::vector<std::pair<BSONObj, ValidateResults>> res;
    if (runForegroundAsWell) {
        res = foregroundValidate(nss, opCtx, valid, numRecords, numInvalidDocuments, numErrors);
    }

    // This function will force a checkpoint, so background validation can then read from that
    // checkpoint.
    // Set 'stableCheckpoint' to false, so we checkpoint ALL data, not just up to WT's
    // stable_timestamp.
    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableTimestamp*/ false);

    ValidateResults validateResults;
    BSONObjBuilder output;
    ASSERT_OK(CollectionValidation::validate(opCtx,
                                             nss,
                                             CollectionValidation::ValidateMode::kBackground,
                                             CollectionValidation::RepairMode::kNone,
                                             &validateResults,
                                             &output));
    BSONObj obj = output.obj();

    ASSERT_EQ(validateResults.valid, valid);
    ASSERT_EQ(validateResults.errors.size(), static_cast<long unsigned int>(numErrors));

    ASSERT_EQ(obj.getIntField("nrecords"), numRecords);
    ASSERT_EQ(obj.getIntField("nInvalidDocuments"), numInvalidDocuments);

    res.push_back({std::make_pair(obj, validateResults)});

    return res;
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
    std::vector<InsertStatement> inserts;
    for (int i = 0; i < numDocs; ++i) {
        BSONObjBuilder bsonBuilder;
        bsonBuilder << "_id" << i + startIDNum;
        for (int c = 1; c <= numFields; ++c) {
            bsonBuilder << "a" + std::to_string(c) << i + (i * numFields + startIDNum) + c;
        }
        const auto obj = bsonBuilder.obj();
        inserts.push_back(InsertStatement(obj));
    }

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(collection_internal::insertDocuments(
            opCtx, *coll, inserts.begin(), inserts.end(), nullptr, false));
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

/**
 * Convenience function to convert ValidateResults to a BSON object.
 */
BSONObj resultToBSON(const ValidateResults& vr) {
    BSONObjBuilder builder;
    vr.appendToResultObj(&builder, true /* debugging */);
    return builder.obj();
}

class CollectionValidationColumnStoreIndexTest : public CollectionValidationDiskTest {
protected:
    CollectionValidationColumnStoreIndexTest() : CollectionValidationDiskTest() {}

    const BSONObj kColumnStoreSpec = BSON("name"
                                          << "$**_columnstore"
                                          << "key"
                                          << BSON("$**"
                                                  << "columnstore")
                                          << "v" << 2);

    /**
     * This method decorates the execution of column-store index validation tests. It works by
     * 1) creating an 'nss' namespace with a column-store index, 2) inserting documents into this
     * namespace (via calling 'insertDocsFn'), 3) running validation on this collection and making
     * sure it's valid, 4) running 'modifyIndexContentsFn', and 5) running 'postCheckFn', which
     * usually contains an index validation call with relevant assertions.
     *
     * Returns the list of validation results.
     */
    std::vector<std::pair<BSONObj, ValidateResults>> runColumnStoreIndexTest(
        const NamespaceString& nss,
        std::function<int(OperationContext*, repl::StorageInterface*)> insertDocsFn,
        std::function<void(OperationContext*, ColumnStore*, int)> modifyIndexContentsFn,
        std::function<std::vector<std::pair<BSONObj, ValidateResults>>(OperationContext*, int)>
            postCheckFn) {

        RAIIServerParameterControllerForTest controller("featureFlagColumnstoreIndexes", true);

        auto opCtx = operationContext();

        {
            // Durable catalog expects metadata updates to be timestamped but this is
            // not necessary in our case - we just want to check the contents of the index table.
            // The alternative here would be to provide a commit timestamp with a TimestamptBlock.
            repl::UnreplicatedWritesBlock uwb(opCtx);

            ASSERT_OK(
                storageInterface()->createIndexesOnEmptyCollection(opCtx, nss, {kColumnStoreSpec}));
        }

        const auto numRecords = insertDocsFn(opCtx, storageInterface());

        // Validate the collection here as a sanity check before we modify the index contents
        // in-place.
        foregroundValidate(nss,
                           opCtx,
                           /*valid*/ true,
                           /*numRecords*/ numRecords,
                           /*numInvalidDocuments*/ 0,
                           /*numErrors*/ 0);

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);

            const auto indexCatalog = autoColl->getIndexCatalog();
            const auto descriptor = indexCatalog->findIndexByName(opCtx, "$**_columnstore");
            ASSERT(descriptor) << "Cannot find $**_columnstore in index catalog";
            const auto entry = indexCatalog->getEntry(descriptor);
            ASSERT(entry) << "Cannot look up index catalog entry for index $**_columnstore";

            const auto columnStore =
                dynamic_cast<ColumnStoreAccessMethod*>(entry->accessMethod())->writableStorage();
            ASSERT_FALSE(columnStore->isEmpty(opCtx))
                << "index $**_columnstore should not be empty";

            modifyIndexContentsFn(opCtx, columnStore, numRecords);
        }

        return postCheckFn(opCtx, numRecords);
    }

    /**
     * Represents a fault where an index entry is deleted.
     */
    struct DeletionFault {};

    /**
     * Represents a fault where an index entry is additionally inserted.
     */
    struct InsertionFault {
        // This value is inserted in the target index cell.
        std::string insertedIndexValue;

        InsertionFault(std::string iVal = "WRONG_KEY") : insertedIndexValue(iVal) {}
    };

    /**
     * Represents a fault where the value of an index entry is replaced with a wrong value.
     */
    struct ReplacementFault {
        // The actual index value is replaced with this given value in the target index cell.
        std::string updatedIndexValue;

        ReplacementFault(std::string uVal = "WRONG_KEY") : updatedIndexValue(uVal) {}
    };

    /**
     * Represents an index corruption in field represented by 'fieldIndex' for a document with id
     * equals to 'docIndex'. The actual fault can be one of the 'DeletionFault', 'InsertionFault',
     * or 'ReplacementFault' options.
     */
    struct InjectedCorruption {
        int fieldIndex;
        int docIndex;
        std::variant<DeletionFault, InsertionFault, ReplacementFault> fault;

        InjectedCorruption(int fieldIdx,
                           int docIdx,
                           std::variant<DeletionFault, InsertionFault, ReplacementFault> f)
            : fieldIndex(fieldIdx), docIndex(docIdx), fault(f) {}


        /**
         * Returns a field index for a field that doesn't exists. As field indexes are assumed to be
         * non-negative, it's guaranteed that a negative field index does not exist. 'fldId' is used
         * to produce more specific negative numbers for the non-existing field index to help with
         * pinpointing the test failures.
         */
        static int getNonExistentFieldIndex(const int fldId) {
            return -fldId;
        }
    };

    int docIndexToRowId(const int docIndex) {
        return docIndex + 1L;
    }

    /**
     * This method runs a column-store index test on 'nss' via a call to 'runColumnStoreIndexTest'.
     * First, it populates an 'nss' collection (with a column-store index defined on it) with
     * 'numDocs' documents, where each document has 'numFields' fields. Then, applies a series of
     * 'corruptions' on the column-store index. Finally, the index validation is ran on this
     * collection (which now has a corrupted column-store index) and the validation results are
     * returned.
     *
     * Note: passing 'doBackgroundValidation = true' performs both foreground and background
     *       validations. However, this can only be done in unit-tests that have a single call to
     *       this method.
     *
     * Returns the list of validation results.
     */
    std::vector<std::pair<BSONObj, ValidateResults>> validateIndexCorruptions(
        const NamespaceString& nss,
        const int numFields,
        const int numDocs,
        const std::vector<InjectedCorruption> corruptions,
        const bool doBackgroundValidation = false) {
        return runColumnStoreIndexTest(
            nss,
            /* insertDocsFn */
            [&](OperationContext* opCtx, repl::StorageInterface* storageInterface) -> int {
                return insertDataRangeForNumFields(
                    nss, opCtx, /*startIDNum*/ 1, /*numDocs*/ numDocs, /*numFields*/ numFields);
            },
            // modifyIndexContentsFn: For each corruption specified, introduces the corruption and
            // then in a separate transaction ensures the corruption is now present.
            [&](OperationContext* opCtx, ColumnStore* columnStore, int numRecords) -> void {
                const auto getPath = [](int corruptedFldIndex) -> std::string {
                    return "a" + std::to_string(corruptedFldIndex);
                };
                const auto seekToCorruptedIndexEntry =
                    [&](int corruptedFldIndex,
                        int corruptedDocIndex) -> boost::optional<FullCellValue> {
                    auto cursor = columnStore->newCursor(opCtx, getPath(corruptedFldIndex));
                    ASSERT(cursor);
                    const auto res =
                        cursor->seekAtOrPast(RowId(docIndexToRowId(corruptedDocIndex)));
                    return res ? FullCellValue(res.value()) : boost::optional<FullCellValue>();
                };

                for (const auto& corruption : corruptions) {
                    const int corruptedFldIndex = corruption.fieldIndex;
                    const int corruptedDocIndex = corruption.docIndex;

                    const auto preCorruptionCell =
                        seekToCorruptedIndexEntry(corruptedFldIndex, corruptedDocIndex);
                    // Apply the requested corruption in a transaction.
                    {
                        WriteUnitOfWork wuow(opCtx);
                        const auto cursor = columnStore->newWriteCursor(opCtx);
                        if (std::holds_alternative<ReplacementFault>(corruption.fault)) {
                            const auto toVal =
                                std::get<ReplacementFault>(corruption.fault).updatedIndexValue;
                            columnStore->update(opCtx,
                                                getPath(corruptedFldIndex),
                                                preCorruptionCell->rid,
                                                StringData(toVal));
                        } else if (std::holds_alternative<DeletionFault>(corruption.fault)) {
                            columnStore->remove(
                                opCtx, getPath(corruptedFldIndex), preCorruptionCell->rid);
                        } else if (std::holds_alternative<InsertionFault>(corruption.fault)) {
                            const auto toVal =
                                std::get<InsertionFault>(corruption.fault).insertedIndexValue;
                            columnStore->insert(opCtx,
                                                getPath(corruptedFldIndex),
                                                RowId(docIndexToRowId(corruptedDocIndex)),
                                                StringData(toVal));
                        } else {
                            MONGO_UNREACHABLE;
                        }
                        wuow.commit();
                    }

                    // Confirm the requested corruption is actually applied (in a separate
                    // transaction).
                    {

                        if (std::holds_alternative<ReplacementFault>(corruption.fault)) {
                            const auto toVal =
                                std::get<ReplacementFault>(corruption.fault).updatedIndexValue;
                            const auto corruptedCell =
                                seekToCorruptedIndexEntry(corruptedFldIndex, corruptedDocIndex);
                            ASSERT_EQ(corruptedCell->path, getPath(corruptedFldIndex));
                            ASSERT_EQ(corruptedCell->rid, preCorruptionCell->rid);
                            ASSERT_EQ(corruptedCell->value, StringData(toVal));
                        } else if (std::holds_alternative<DeletionFault>(corruption.fault)) {
                            const auto corruptedCell =
                                seekToCorruptedIndexEntry(corruptedFldIndex, corruptedDocIndex);
                            if (numDocs == 1 || corruptedDocIndex == numDocs - 1) {
                                ASSERT_FALSE(corruptedCell);
                            } else {
                                ASSERT_EQ(corruptedCell->path, getPath(corruptedFldIndex));
                                ASSERT_GT(corruptedCell->rid, preCorruptionCell->rid);
                            }
                        } else if (std::holds_alternative<InsertionFault>(corruption.fault)) {
                            const auto toVal =
                                std::get<InsertionFault>(corruption.fault).insertedIndexValue;
                            const auto corruptedCell =
                                seekToCorruptedIndexEntry(corruptedFldIndex, corruptedDocIndex);
                            ASSERT_EQ(corruptedCell->path, getPath(corruptedFldIndex));
                            ASSERT_EQ(corruptedCell->rid,
                                      RowId(docIndexToRowId(corruptedDocIndex)));
                            ASSERT_EQ(corruptedCell->value, StringData(toVal));
                        } else {
                            MONGO_UNREACHABLE;
                        }
                    }
                }
            },
            /* postCheckFn */
            [&](OperationContext* opCtx,
                int numRecords) -> std::vector<std::pair<BSONObj, ValidateResults>> {
                auto serviceContext = opCtx->getServiceContext();

                // Confirm there is an expected validation error
                std::vector<std::pair<BSONObj, ValidateResults>> results;

                if (doBackgroundValidation) {
                    // Background validation must be done in a separate thread due to the
                    // assumptions made in its implementation.
                    stdx::thread runBackgroundValidate =
                        stdx::thread([&serviceContext, &numRecords, &nss, &results] {
                            ThreadClient tc("BackgroundValidate-thread", serviceContext);
                            auto threadOpCtx = tc->makeOperationContext();
                            auto bgResults = backgroundValidate(nss,
                                                                threadOpCtx.get(),
                                                                /*valid*/ false,
                                                                /*numRecords*/ numRecords,
                                                                /*numInvalidDocuments*/ 0,
                                                                /*numErrors*/ 1,
                                                                /*runForegroundAsWell*/ false);
                            results.insert(results.end(), bgResults.begin(), bgResults.end());
                        });
                    // Make sure the background validation finishes successfully.
                    runBackgroundValidate.join();
                }

                const auto fgResults = foregroundValidate(nss,
                                                          opCtx,
                                                          /*valid*/ false,
                                                          /*numRecords*/ numRecords,
                                                          /*numInvalidDocuments*/ 0,
                                                          /*numErrors*/ 1);

                results.insert(results.end(), fgResults.begin(), fgResults.end());

                return results;
            });
    }

    /**
     * This method repairs the (possible) index corruptions by running the validate command and
     * returns the results from this command (after doing some common assertions). In addition,
     * before returning this result, another call to the validate command is done to repair the
     * index again. It's expected that this second call to index repair will not lead to any repair,
     * as it's done in the first call (if any corruption exists).
     */
    std::pair<BSONObj, ValidateResults> repairIndexCorruptions(const NamespaceString& nss,
                                                               int numDocs) {
        auto opCtx = operationContext();
        const auto repairResults1 =
            foregroundValidate(nss,
                               opCtx,
                               /*valid*/ true,
                               /*numRecords*/ numDocs,
                               /*numInvalidDocuments*/ 0,
                               /*numErrors*/ 0,
                               {CollectionValidation::ValidateMode::kForeground},
                               CollectionValidation::RepairMode::kFixErrors);

        ASSERT_EQ(repairResults1.size(), 1);

        const auto& repairResult1 = repairResults1[0];
        const auto& validateResults1 = repairResult1.second;
        auto obj = resultToBSON(validateResults1);
        ASSERT(validateResults1.valid) << obj;

        ASSERT_EQ(validateResults1.missingIndexEntries.size(), 0U) << obj;
        ASSERT_EQ(validateResults1.extraIndexEntries.size(), 0U) << obj;
        ASSERT_EQ(validateResults1.corruptRecords.size(), 0U) << obj;
        ASSERT_EQ(validateResults1.numRemovedCorruptRecords, 0U) << obj;
        ASSERT_EQ(validateResults1.numDocumentsMovedToLostAndFound, 0U) << obj;
        ASSERT_EQ(validateResults1.numOutdatedMissingIndexEntry, 0U) << obj;

        // After the first round of repair, if we do a second round of repair, it should always
        // validate without requiring any actual repair anymore.
        const auto repairResults2 =
            foregroundValidate(nss,
                               opCtx,
                               /*valid*/ true,
                               /*numRecords*/ numDocs,
                               /*numInvalidDocuments*/ 0,
                               /*numErrors*/ 0,
                               {CollectionValidation::ValidateMode::kForeground},
                               CollectionValidation::RepairMode::kFixErrors);
        ASSERT_EQ(repairResults2.size(), 1);

        const auto& validateResults2 = repairResults2[0].second;
        obj = resultToBSON(validateResults2);
        ASSERT(validateResults2.valid) << obj;
        ASSERT_FALSE(validateResults2.repaired) << obj;

        const auto warningsWithoutTransientErrors = omitTransientWarnings(validateResults2);
        ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 0U) << obj;
        ASSERT_EQ(validateResults2.missingIndexEntries.size(), 0U) << obj;
        ASSERT_EQ(validateResults2.extraIndexEntries.size(), 0U) << obj;
        ASSERT_EQ(validateResults2.corruptRecords.size(), 0U) << obj;
        ASSERT_EQ(validateResults2.numRemovedCorruptRecords, 0U) << obj;
        ASSERT_EQ(validateResults2.numRemovedExtraIndexEntries, 0U) << obj;
        ASSERT_EQ(validateResults2.numInsertedMissingIndexEntries, 0U) << obj;
        ASSERT_EQ(validateResults2.numDocumentsMovedToLostAndFound, 0U) << obj;
        ASSERT_EQ(validateResults2.numOutdatedMissingIndexEntry, 0U) << obj;

        // return the result of initial repair command, so that tests can do further checks on that
        // result if needed.
        return repairResult1;
    }
};

// Verify that calling validate() on an empty collection with different validation levels returns an
// OK status.
TEST_F(CollectionValidationTest, ValidateEmpty) {
    foregroundValidate(kNss,
                       operationContext(),
                       /*valid*/ true,
                       /*numRecords*/ 0,
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0);
}
TEST_F(CollectionValidationDiskTest, BackgroundValidateEmpty) {
    backgroundValidate(kNss,
                       operationContext(),
                       /*valid*/ true,
                       /*numRecords*/ 0,
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0,
                       /*runForegroundAsWell*/ true);
}

// Verify calling validate() on a nonempty collection with different validation levels.
TEST_F(CollectionValidationTest, Validate) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       /*valid*/ true,
                       /*numRecords*/ insertDataRange(opCtx, 0, 5),
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0);
}
TEST_F(CollectionValidationDiskTest, BackgroundValidate) {
    auto opCtx = operationContext();
    backgroundValidate(kNss,
                       opCtx,
                       /*valid*/ true,
                       /*numRecords*/ insertDataRange(opCtx, 0, 5),
                       /*numInvalidDocuments*/ 0,
                       /*numErrors*/ 0,
                       /*runForegroundAsWell*/ true);
}

// Verify calling validate() on a collection with an invalid document.
TEST_F(CollectionValidationTest, ValidateError) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
                       /*valid*/ false,
                       /*numRecords*/ setUpInvalidData(opCtx),
                       /*numInvalidDocuments*/ 1,
                       /*numErrors*/ 1);
}
TEST_F(CollectionValidationDiskTest, BackgroundValidateError) {
    auto opCtx = operationContext();
    backgroundValidate(kNss,
                       opCtx,
                       /*valid*/ false,
                       /*numRecords*/ setUpInvalidData(opCtx),
                       /*numInvalidDocuments*/ 1,
                       /*numErrors*/ 1,
                       /*runForegroundAsWell*/ true);
}

// Verify calling validate() with enforceFastCount=true.
TEST_F(CollectionValidationTest, ValidateEnforceFastCount) {
    auto opCtx = operationContext();
    foregroundValidate(kNss,
                       opCtx,
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
                kNss, threadOpCtx.get(), true, numRecords, 0, 0, /*runForegroundAsWell*/ false);
        });

        // Wait until validate starts and hangs mid-way on a failpoint, then do concurrent writes,
        // which should succeed and not affect the background validation.
        waitUntilValidateFailpointHasBeenReached();
        numRecords2 = insertDataRange(opCtx, 5, 15);
    }

    // Make sure the background validation finishes successfully.
    runBackgroundValidate.join();

    // Run regular foreground collection validation to make sure everything is OK.
    foregroundValidate(kNss,
                       opCtx,
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
        kNss, opCtx, /*valid*/ true, /*numRecords*/ 1, /*numInvalidDocuments*/ 0, /*numErrors*/ 0);

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

    const auto results = foregroundValidate(kNss,
                                            opCtx,
                                            /*valid*/ true,
                                            /*numRecords*/ 1,
                                            /*numInvalidDocuments*/ 0,
                                            /*numErrors*/ 0);
    ASSERT_EQ(results.size(), 2);

    for (const auto& result : results) {
        const auto& validateResults = result.second;
        const auto obj = resultToBSON(validateResults);
        ASSERT(validateResults.valid) << obj;
        const auto warningsWithoutTransientErrors = omitTransientWarnings(validateResults);
        ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 1U) << obj;
        ASSERT_STRING_CONTAINS(warningsWithoutTransientErrors.warnings[0],
                               "Unique index a_1 has one or more keys in the old format")
            << obj;
    }
}

/**
 * Checks whether a given 'entry' is equal to any element in the 'list'.
 */
bool equalsAny(const std::string& entry, const std::vector<std::string>& list) {
    return std::any_of(
        list.begin(), list.end(), [&entry](const std::string& other) { return entry == other; });
}

// Exhaustively tests having one error in the column-store index by updating one index entry with an
// invalid value in different parts of the index on collections with different number of columns and
// documents.
TEST_F(CollectionValidationColumnStoreIndexTest, SingleInvalidIndexEntryCSI) {
    const int kNumFields = 4;
    const int kMaxNumDocs = 4;

    int testCaseIdx = 0;
    for (int numFields = 1; numFields <= kNumFields; ++numFields) {
        for (int numDocs = 1; numDocs <= kMaxNumDocs; ++numDocs) {
            for (int corruptedFldIndex = 1; corruptedFldIndex <= numFields; ++corruptedFldIndex) {
                for (int corruptedDocIndex = 0; corruptedDocIndex < numDocs; ++corruptedDocIndex) {
                    const NamespaceString nss(kNss.toString() + std::to_string(++testCaseIdx));

                    // Create collection nss for unit tests to use.
                    const CollectionOptions defaultCollectionOptions;
                    ASSERT_OK(storageInterface()->createCollection(
                        operationContext(), nss, defaultCollectionOptions));

                    const auto results = validateIndexCorruptions(
                        nss,
                        numFields,
                        numDocs,
                        /* column-store index corruptions */
                        {{corruptedFldIndex,
                          corruptedDocIndex,
                          /* Update the current index entry with an invalid value. */
                          ReplacementFault("WRONG_" + std::to_string(corruptedFldIndex) + "_" +
                                           std::to_string(corruptedDocIndex))}},
                        /* doBackgroundValidation */ true);

                    ASSERT_EQ(results.size(), 3);

                    for (const auto& result : results) {
                        const auto& validateResults = result.second;
                        const auto obj = resultToBSON(validateResults);
                        ASSERT_FALSE(validateResults.valid) << obj;

                        const auto warningsWithoutTransientErrors =
                            omitTransientWarnings(validateResults);
                        ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 2U) << obj;
                        ASSERT(equalsAny("Detected 1 missing index entries.",
                                         warningsWithoutTransientErrors.warnings))
                            << obj;
                        ASSERT(equalsAny("Detected 1 extra index entries.",
                                         warningsWithoutTransientErrors.warnings))
                            << obj;

                        ASSERT_EQ(validateResults.errors.size(), 1U) << obj;
                        ASSERT_EQ(validateResults.errors[0],
                                  "Index with name '$**_columnstore' has inconsistencies.")
                            << obj;

                        const auto& extraEntries = validateResults.extraIndexEntries;
                        ASSERT_EQ(extraEntries.size(), 1U) << obj;
                        ASSERT_EQ(extraEntries[0]["indexName"].String(), "$**_columnstore") << obj;
                        ASSERT_EQ(extraEntries[0]["recordId"].Long(),
                                  docIndexToRowId(corruptedDocIndex))
                            << obj;
                        ASSERT_EQ(extraEntries[0]["rowId"].Long(),
                                  docIndexToRowId(corruptedDocIndex))
                            << obj;
                        ASSERT_EQ(extraEntries[0]["indexPath"].String(),
                                  "a" + std::to_string(corruptedFldIndex))
                            << obj;

                        const auto& missingEntries = validateResults.missingIndexEntries;
                        ASSERT_EQ(missingEntries.size(), 1U) << obj;
                        ASSERT_EQ(missingEntries[0]["indexName"].String(), "$**_columnstore")
                            << obj;
                        ASSERT_EQ(missingEntries[0]["recordId"].Long(),
                                  docIndexToRowId(corruptedDocIndex))
                            << obj;
                        ASSERT_EQ(missingEntries[0]["rowId"].Long(),
                                  docIndexToRowId(corruptedDocIndex))
                            << obj;
                        ASSERT_EQ(missingEntries[0]["indexPath"].String(),
                                  "a" + std::to_string(corruptedFldIndex))
                            << obj;

                        ASSERT_EQ(validateResults.corruptRecords.size(), 0U) << obj;
                        ASSERT_EQ(validateResults.numRemovedCorruptRecords, 0U) << obj;
                        ASSERT_EQ(validateResults.numRemovedExtraIndexEntries, 0U) << obj;
                        ASSERT_EQ(validateResults.numInsertedMissingIndexEntries, 0U) << obj;
                        ASSERT_EQ(validateResults.numDocumentsMovedToLostAndFound, 0U) << obj;
                        ASSERT_EQ(validateResults.numOutdatedMissingIndexEntry, 0U) << obj;
                    }

                    const auto& repairResult = repairIndexCorruptions(nss, numDocs);
                    const auto& validateResults = repairResult.second;
                    const auto obj = resultToBSON(validateResults);
                    ASSERT(validateResults.valid) << obj;
                    ASSERT(validateResults.repaired) << obj;

                    const auto warningsWithoutTransientErrors =
                        omitTransientWarnings(validateResults);
                    ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 1U) << obj;
                    ASSERT_STRING_CONTAINS(warningsWithoutTransientErrors.warnings[0],
                                           "Inserted 1 missing index entries.")
                        << obj;
                    ASSERT_EQ(validateResults.numRemovedExtraIndexEntries, 1U) << obj;
                    ASSERT_EQ(validateResults.numInsertedMissingIndexEntries, 1U) << obj;
                }
            }
        }
    }
}

// Exhaustively tests having one error in the column-store index by adding an extra index entry on
// collections with different number of columns and documents.
TEST_F(CollectionValidationColumnStoreIndexTest, SingleExtraIndexEntry) {
    const int kNumFields = 4;
    const int kMaxNumDocs = 4;

    std::vector<std::pair<int, int>> extraIndexEntryCorruptions = {
        /* non-existent field on an existing document */
        {/*corruptedFldIndex*/ InjectedCorruption::getNonExistentFieldIndex(1),
         /*corruptedDocIndex*/ 0},
        /* non-existent field on a non-existent document */
        {/*corruptedFldIndex*/ InjectedCorruption::getNonExistentFieldIndex(2),
         /*corruptedDocIndex*/ kMaxNumDocs * 10}};
    for (int corruptedFldIndex = 1; corruptedFldIndex <= kNumFields; ++corruptedFldIndex) {
        /* existing field on a non-existent document */
        extraIndexEntryCorruptions.push_back(
            {corruptedFldIndex, /*corruptedDocIndex*/ kMaxNumDocs * 10});
    }

    int testCaseIdx = 0;
    for (int numFields = 1; numFields <= kNumFields; ++numFields) {
        for (int numDocs = 1; numDocs <= kMaxNumDocs; ++numDocs) {
            for (const auto& corruption : extraIndexEntryCorruptions) {
                const int corruptedFldIndex = corruption.first;
                const int corruptedDocIndex = corruption.second;

                const auto nss = NamespaceString(kNss.toString() + std::to_string(++testCaseIdx));

                // Create collection nss for unit tests to use.
                const CollectionOptions defaultCollectionOptions;
                ASSERT_OK(storageInterface()->createCollection(
                    operationContext(), nss, defaultCollectionOptions));

                const auto results = validateIndexCorruptions(
                    nss,
                    numFields,
                    numDocs,
                    /* column-store index corruptions */
                    {{corruptedFldIndex,
                      corruptedDocIndex,
                      /* Insert an extra index entry. */
                      InsertionFault("WRONG_" + std::to_string(corruptedFldIndex) + "_" +
                                     std::to_string(corruptedDocIndex))}},
                    /* doBackgroundValidation */ true);

                ASSERT_EQ(results.size(), 3);

                for (const auto& result : results) {
                    const auto& validateResults = result.second;
                    const auto obj = resultToBSON(validateResults);
                    ASSERT_FALSE(validateResults.valid) << obj;

                    const auto warningsWithoutTransientErrors =
                        omitTransientWarnings(validateResults);
                    ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 1U) << obj;
                    ASSERT(fmt::format("Detected {} extra index entries.", 1) ==
                           warningsWithoutTransientErrors.warnings[0])
                        << obj;

                    ASSERT_EQ(validateResults.errors.size(), 1U) << obj;
                    ASSERT_EQ(validateResults.errors[0],
                              "Index with name '$**_columnstore' has inconsistencies.")
                        << obj;

                    const auto& extraEntries = validateResults.extraIndexEntries;
                    ASSERT_EQ(extraEntries.size(), 1) << obj;
                    ASSERT_EQ(extraEntries[0]["indexName"].String(), "$**_columnstore") << obj;
                    ASSERT_EQ(extraEntries[0]["recordId"].Long(),
                              docIndexToRowId(corruptedDocIndex))
                        << obj;
                    ASSERT_EQ(extraEntries[0]["rowId"].Long(), docIndexToRowId(corruptedDocIndex))
                        << obj;
                    ASSERT_EQ(extraEntries[0]["indexPath"].String(),
                              "a" + std::to_string(corruptedFldIndex))
                        << obj;

                    ASSERT_EQ(validateResults.missingIndexEntries.size(), 0U) << obj;
                    ASSERT_EQ(validateResults.corruptRecords.size(), 0U) << obj;
                    ASSERT_EQ(validateResults.numRemovedCorruptRecords, 0U) << obj;
                    ASSERT_EQ(validateResults.numRemovedExtraIndexEntries, 0U) << obj;
                    ASSERT_EQ(validateResults.numInsertedMissingIndexEntries, 0U) << obj;
                    ASSERT_EQ(validateResults.numDocumentsMovedToLostAndFound, 0U) << obj;
                    ASSERT_EQ(validateResults.numOutdatedMissingIndexEntry, 0U) << obj;
                }

                {
                    const auto& repairResult = repairIndexCorruptions(nss, numDocs);
                    const auto& validateResults = repairResult.second;
                    const auto obj = resultToBSON(validateResults);
                    ASSERT(validateResults.repaired) << obj;

                    const auto warningsWithoutTransientErrors =
                        omitTransientWarnings(validateResults);
                    ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 0U) << obj;
                    ASSERT_EQ(validateResults.numRemovedExtraIndexEntries, 1U) << obj;
                    ASSERT_EQ(validateResults.numInsertedMissingIndexEntries, 0U) << obj;
                }
            }
        }
    }
}

// Exhaustively tests having one error in the column-store index by removing one index entry from
// different parts of the index on collections with different number of columns and documents.
TEST_F(CollectionValidationColumnStoreIndexTest, SingleMissingIndexEntryCSI) {
    const int kNumFields = 4;
    const int kMaxNumDocs = 4;

    int testCaseIdx = 0;
    for (int numFields = 1; numFields <= kNumFields; ++numFields) {
        for (int numDocs = 1; numDocs <= kMaxNumDocs; ++numDocs) {
            for (int corruptedFldIndex = 1; corruptedFldIndex <= numFields; ++corruptedFldIndex) {
                for (int corruptedDocIndex = 0; corruptedDocIndex < numDocs; ++corruptedDocIndex) {
                    const NamespaceString nss(kNss.toString() + std::to_string(++testCaseIdx));

                    // Create collection nss for unit tests to use.
                    const CollectionOptions defaultCollectionOptions;
                    ASSERT_OK(storageInterface()->createCollection(
                        operationContext(), nss, defaultCollectionOptions));

                    const auto results =
                        validateIndexCorruptions(nss,
                                                 numFields,
                                                 numDocs,
                                                 /* column-store index corruptions */
                                                 {{corruptedFldIndex,
                                                   corruptedDocIndex,
                                                   /* Remove the existing index entry. */
                                                   DeletionFault()}},
                                                 /* doBackgroundValidation */ true);

                    ASSERT_EQ(results.size(), 3);

                    for (const auto& result : results) {
                        const auto& validateResults = result.second;
                        const auto obj = resultToBSON(validateResults);
                        ASSERT_FALSE(validateResults.valid) << obj;

                        const auto warningsWithoutTransientErrors =
                            omitTransientWarnings(validateResults);
                        ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 1U) << obj;
                        ASSERT("Detected 1 missing index entries." ==
                               warningsWithoutTransientErrors.warnings[0])
                            << obj;

                        ASSERT_EQ(validateResults.errors.size(), 1U) << obj;
                        ASSERT_EQ(validateResults.errors[0],
                                  "Index with name '$**_columnstore' has inconsistencies.")
                            << obj;

                        const auto& missingEntries = validateResults.missingIndexEntries;
                        ASSERT_EQ(missingEntries.size(), 1U) << obj;
                        ASSERT_EQ(missingEntries[0]["indexName"].String(), "$**_columnstore")
                            << obj;
                        ASSERT_EQ(missingEntries[0]["recordId"].Long(),
                                  docIndexToRowId(corruptedDocIndex))
                            << obj;
                        ASSERT_EQ(missingEntries[0]["rowId"].Long(),
                                  docIndexToRowId(corruptedDocIndex))
                            << obj;
                        ASSERT_EQ(missingEntries[0]["indexPath"].String(),
                                  "a" + std::to_string(corruptedFldIndex))
                            << obj;

                        ASSERT_EQ(validateResults.extraIndexEntries.size(), 0U) << obj;
                        ASSERT_EQ(validateResults.corruptRecords.size(), 0U) << obj;
                        ASSERT_EQ(validateResults.numRemovedCorruptRecords, 0U) << obj;
                        ASSERT_EQ(validateResults.numRemovedExtraIndexEntries, 0U) << obj;
                        ASSERT_EQ(validateResults.numInsertedMissingIndexEntries, 0U) << obj;
                        ASSERT_EQ(validateResults.numDocumentsMovedToLostAndFound, 0U) << obj;
                        ASSERT_EQ(validateResults.numOutdatedMissingIndexEntry, 0U) << obj;
                    }

                    {
                        const auto& repairResult = repairIndexCorruptions(nss, numDocs);
                        const auto& validateResults = repairResult.second;
                        const auto obj = resultToBSON(validateResults);
                        ASSERT(validateResults.repaired) << obj;

                        const auto warningsWithoutTransientErrors =
                            omitTransientWarnings(validateResults);
                        ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 1U) << obj;
                        ASSERT_STRING_CONTAINS(warningsWithoutTransientErrors.warnings[0],
                                               "Inserted 1 missing index entries.")
                            << obj;
                        ASSERT_EQ(validateResults.numRemovedExtraIndexEntries, 0U) << obj;
                        ASSERT_EQ(validateResults.numInsertedMissingIndexEntries, 1U) << obj;
                    }
                }
            }
        }
    }
}

// Tests having multiple errors in the column-store index by updating several index entries with an
// invalid value in different parts of the index on a collection with multiple columns and multiple
// documents.
TEST_F(CollectionValidationColumnStoreIndexTest, MultipleInvalidIndexEntryCSI) {
    const int numFields = 10;
    const int numDocs = 50;

    auto results = validateIndexCorruptions(
        kNss,
        numFields,
        numDocs,
        /* column-store index corruptions */
        {{/* corruptedFldIndex */ 5,
          /* corruptedDocIndex */ 9,
          /* Remove the existing index entry. */
          DeletionFault()},
         {/* corruptedFldIndex */ 7,
          /* corruptedDocIndex */ 2,
          /* Update the current index entry with an invalid value. */
          ReplacementFault()},
         {/* corruptedFldIndex */ 9,
          /* corruptedDocIndex */ 500,
          /* Insert an extra index entry for a non-exisiting document. */
          InsertionFault()},
         {/* corruptedFldIndex */ InjectedCorruption::getNonExistentFieldIndex(1),
          /* corruptedDocIndex */ 5,
          /* Insert an extra index entry for a non-existing field of an existing document. */
          InsertionFault()},
         {/* corruptedFldIndex */ InjectedCorruption::getNonExistentFieldIndex(2),
          /* corruptedDocIndex */ 600,
          /* Insert an extra index entry for a non-existing field of a non-existing document. */
          InsertionFault()},
         {/* corruptedFldIndex */ 2,
          /* corruptedDocIndex */ 33,
          /* Update the current index entry with an invalid value. */
          ReplacementFault()}},
        /* doBackgroundValidation */ true);

    ASSERT_EQ(results.size(), 3);

    for (const auto& result : results) {
        const auto& validateResults = result.second;
        const auto obj = resultToBSON(validateResults);
        ASSERT_FALSE(validateResults.valid) << obj;

        const auto warningsWithoutTransientErrors = omitTransientWarnings(validateResults);
        ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 2U) << obj;
        ASSERT(
            equalsAny("Detected 3 missing index entries.", warningsWithoutTransientErrors.warnings))
            << obj;
        ASSERT(
            equalsAny("Detected 5 extra index entries.", warningsWithoutTransientErrors.warnings))
            << obj;

        ASSERT_EQ(validateResults.errors.size(), 1U) << obj;
        ASSERT_EQ(validateResults.errors[0],
                  "Index with name '$**_columnstore' has inconsistencies.")
            << obj;

        ASSERT_EQ(validateResults.missingIndexEntries.size(), 3U) << obj;
        ASSERT_EQ(validateResults.extraIndexEntries.size(), 5U) << obj;
        ASSERT_EQ(validateResults.corruptRecords.size(), 0U) << obj;
        ASSERT_EQ(validateResults.numRemovedCorruptRecords, 0U) << obj;
        ASSERT_EQ(validateResults.numRemovedExtraIndexEntries, 0U) << obj;
        ASSERT_EQ(validateResults.numInsertedMissingIndexEntries, 0U) << obj;
        ASSERT_EQ(validateResults.numDocumentsMovedToLostAndFound, 0U) << obj;
        ASSERT_EQ(validateResults.numOutdatedMissingIndexEntry, 0U) << obj;
    }

    {
        const auto& repairResult = repairIndexCorruptions(kNss, numDocs);
        const auto& validateResults = repairResult.second;
        const auto obj = resultToBSON(validateResults);
        ASSERT(validateResults.repaired) << obj;

        const auto warningsWithoutTransientErrors = omitTransientWarnings(validateResults);
        ASSERT_EQ(warningsWithoutTransientErrors.warnings.size(), 1U) << obj;
        ASSERT_STRING_CONTAINS(warningsWithoutTransientErrors.warnings[0],
                               "Inserted 3 missing index entries.")
            << obj;
        ASSERT_EQ(validateResults.numRemovedExtraIndexEntries, 5U) << obj;
        ASSERT_EQ(validateResults.numInsertedMissingIndexEntries, 3U) << obj;
    }
}

}  // namespace
}  // namespace mongo

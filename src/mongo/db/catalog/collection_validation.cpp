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

#include "mongo/db/catalog/collection_validation.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {

using logger::LogComponent;
using std::string;

MONGO_FAIL_POINT_DEFINE(pauseCollectionValidationWithLock);

namespace CollectionValidation {

namespace {

using ValidateResultsMap = std::map<string, ValidateResults>;

// Indicates whether the failpoint turned on by testing has been reached.
AtomicWord<bool> _validationIsPausedForTest{false};

/**
 * Opens a cursor on each index in the given 'indexCatalog'.
 *
 * Returns a map from indexName -> indexCursor.
 */
std::map<std::string, std::unique_ptr<SortedDataInterfaceThrottleCursor>> _openIndexCursors(
    OperationContext* opCtx,
    IndexCatalog* indexCatalog,
    std::shared_ptr<DataThrottle> dataThrottle) {
    std::map<std::string, std::unique_ptr<SortedDataInterfaceThrottleCursor>> indexCursors;
    const std::unique_ptr<IndexCatalog::IndexIterator> it =
        indexCatalog->getIndexIterator(opCtx, false);
    while (it->more()) {
        const IndexCatalogEntry* entry = it->next();
        indexCursors.emplace(entry->descriptor()->indexName(),
                             std::make_unique<SortedDataInterfaceThrottleCursor>(
                                 opCtx, entry->accessMethod(), dataThrottle));
    }
    return indexCursors;
}

/**
 * Validates the internal structure of each index in the Index Catalog 'indexCatalog', ensuring that
 * the index files have not been corrupted or compromised.
 *
 * May close or invalidate open cursors.
 *
 * Returns a map from indexName -> number of keys validated.
 */
std::map<std::string, int64_t> _validateIndexesInternalStructure(
    OperationContext* opCtx,
    IndexCatalog* indexCatalog,
    ValidateResultsMap* indexNsResultsMap,
    ValidateResults* results) {
    std::map<std::string, int64_t> numIndexKeysPerIndex;
    const std::unique_ptr<IndexCatalog::IndexIterator> it =
        indexCatalog->getIndexIterator(opCtx, false);

    // Validate Indexes Internal Structure, checking if index files have been compromised or
    // corrupted.
    while (it->more()) {
        opCtx->checkForInterrupt();

        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();

        log(LogComponent::kIndex) << "validating the internal structure of index "
                                  << descriptor->indexName() << " on collection "
                                  << descriptor->parentNS();
        ValidateResults& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];

        int64_t numValidated;
        iam->validate(opCtx, &numValidated, &curIndexResults);

        numIndexKeysPerIndex[descriptor->indexName()] = numValidated;
    }
    return numIndexKeysPerIndex;
}

/**
 * Validates each index in the Index Catalog using the cursors in 'indexCursors'.
 *
 * If 'level' is kValidateFull, then we will compare new index entry counts with a previously taken
 * count saved in 'numIndexKeysPerIndex'.
 */
void _validateIndexes(
    OperationContext* opCtx,
    IndexCatalog* indexCatalog,
    BSONObjBuilder* keysPerIndex,
    ValidateAdaptor* indexValidator,
    ValidateCmdLevel level,
    const std::map<std::string, std::unique_ptr<SortedDataInterfaceThrottleCursor>>& indexCursors,
    const std::map<std::string, int64_t>& numIndexKeysPerIndex,
    ValidateResultsMap* indexNsResultsMap,
    ValidateResults* results) {

    const std::unique_ptr<IndexCatalog::IndexIterator> it =
        indexCatalog->getIndexIterator(opCtx, false);

    // Validate Indexes, checking for mismatch between index entries and collection records.
    while (it->more()) {
        opCtx->checkForInterrupt();

        const IndexDescriptor* descriptor = it->next()->descriptor();

        log(LogComponent::kIndex) << "validating index consistency " << descriptor->indexName()
                                  << " on collection " << descriptor->parentNS();

        // Ensure that this index had an index cursor opened in _openIndexCursors.
        const auto indexCursorIt = indexCursors.find(descriptor->indexName());
        invariant(indexCursorIt != indexCursors.end());

        ValidateResults& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];
        int64_t numTraversedKeys;
        indexValidator->traverseIndex(
            opCtx, &numTraversedKeys, indexCursorIt->second, descriptor, &curIndexResults);

        // If we are performing a full validation, we have information on the number of index keys
        // validated in _validateIndexesInternalStructure (when we validated the internal structure
        // of the index). Check if this is consistent with 'numTraversedKeys' from traverseIndex
        // above.
        if (level == kValidateFull) {
            invariant(
                opCtx->lockState()->isCollectionLockedForMode(descriptor->parentNS(), MODE_X));

            // Ensure that this index was validated in _validateIndexesInternalStructure.
            const auto numIndexKeysIt = numIndexKeysPerIndex.find(descriptor->indexName());
            invariant(numIndexKeysIt != numIndexKeysPerIndex.end());

            // The number of keys counted in _validateIndexesInternalStructure, when checking the
            // internal structure of the index.
            const int64_t numIndexKeys = numIndexKeysIt->second;

            // Check if currIndexResults is valid to ensure that this index is not corrupted or
            // comprised (which was set in _validateIndexesInternalStructure). If the index is
            // corrupted, there is no use in checking if the traversal yielded the same key count.
            if (curIndexResults.valid) {
                if (numIndexKeys != numTraversedKeys) {
                    curIndexResults.valid = false;
                    string msg = str::stream()
                        << "number of traversed index entries (" << numTraversedKeys
                        << ") does not match the number of expected index entries (" << numIndexKeys
                        << ")";
                    results->errors.push_back(msg);
                    results->valid = false;
                }
            }
        }

        if (curIndexResults.valid) {
            keysPerIndex->appendNumber(descriptor->indexName(),
                                       static_cast<long long>(numTraversedKeys));
        } else {
            results->valid = false;
        }
    }
}

/**
 * Executes the second phase of validation for improved error reporting. This is only done if
 * any index inconsistencies are found during the first phase of validation.
 */
void _gatherIndexEntryErrors(
    OperationContext* opCtx,
    Collection* coll,
    IndexConsistency* indexConsistency,
    ValidateAdaptor* indexValidator,
    const RecordId& firstRecordId,
    const std::unique_ptr<SeekableRecordThrottleCursor>& traverseRecordStoreCursor,
    const std::unique_ptr<SeekableRecordThrottleCursor>& seekRecordStoreCursor,
    const std::map<std::string, std::unique_ptr<SortedDataInterfaceThrottleCursor>>& indexCursors,
    ValidateResultsMap* indexNsResultsMap,
    ValidateResults* result) {
    indexConsistency->setSecondPhase();

    log(LogComponent::kIndex) << "Starting to traverse through all the document key sets.";

    // During the second phase of validation, iterate through each documents key set and only record
    // the keys that were inconsistent during the first phase of validation.
    for (auto record = traverseRecordStoreCursor->seekExact(opCtx, firstRecordId); record;
         record = traverseRecordStoreCursor->next(opCtx)) {
        opCtx->checkForInterrupt();

        // We can ignore the status of validate as it was already checked during the first phase.
        size_t validatedSize;
        indexValidator
            ->validateRecord(
                opCtx, coll, record->id, record->data, seekRecordStoreCursor, &validatedSize)
            .ignore();
    }

    log(LogComponent::kIndex) << "Finished traversing through all the document key sets.";
    log(LogComponent::kIndex) << "Starting to traverse through all the indexes.";

    // Iterate through all the indexes in the collection and only record the index entry keys that
    // had inconsistencies during the first phase.
    std::unique_ptr<IndexCatalog::IndexIterator> it =
        coll->getIndexCatalog()->getIndexIterator(opCtx, false);
    while (it->more()) {
        opCtx->checkForInterrupt();

        const IndexDescriptor* descriptor = it->next()->descriptor();

        log(LogComponent::kIndex) << "Traversing through the index entries for index "
                                  << descriptor->indexName() << ".";

        // Ensure that this index had an index cursor opened in _openIndexCursors.
        const auto indexCursorIt = indexCursors.find(descriptor->indexName());
        invariant(indexCursorIt != indexCursors.end());

        indexValidator->traverseIndex(opCtx,
                                      /*numTraversedKeys=*/nullptr,
                                      indexCursorIt->second,
                                      descriptor,
                                      /*ValidateResults=*/nullptr);
    }

    log(LogComponent::kIndex) << "Finished traversing through all the indexes.";

    indexConsistency->addIndexEntryErrors(indexNsResultsMap, result);
}

void _validateIndexKeyCount(OperationContext* opCtx,
                            Collection* coll,
                            ValidateAdaptor* indexValidator,
                            ValidateResultsMap* indexNsResultsMap) {

    const std::unique_ptr<IndexCatalog::IndexIterator> indexIterator =
        coll->getIndexCatalog()->getIndexIterator(opCtx, false);
    while (indexIterator->more()) {
        const IndexDescriptor* descriptor = indexIterator->next()->descriptor();
        ValidateResults& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];

        if (curIndexResults.valid) {
            indexValidator->validateIndexKeyCount(
                descriptor, coll->getRecordStore()->numRecords(opCtx), curIndexResults);
        }
    }
}

void _reportValidationResults(OperationContext* opCtx,
                              Collection* collection,
                              ValidateResultsMap* indexNsResultsMap,
                              BSONObjBuilder* keysPerIndex,
                              ValidateCmdLevel level,
                              ValidateResults* results,
                              BSONObjBuilder* output) {
    std::unique_ptr<BSONObjBuilder> indexDetails;
    if (level == kValidateFull) {
        invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));
        indexDetails = std::make_unique<BSONObjBuilder>();
    }

    // Report index validation results.
    for (const auto& it : *indexNsResultsMap) {
        const string indexName = it.first;
        const ValidateResults& vr = it.second;

        if (!vr.valid) {
            results->valid = false;
        }

        if (indexDetails) {
            BSONObjBuilder bob(indexDetails->subobjStart(indexName));
            bob.appendBool("valid", vr.valid);

            if (!vr.warnings.empty()) {
                bob.append("warnings", vr.warnings);
            }

            if (!vr.errors.empty()) {
                bob.append("errors", vr.errors);
            }
        }

        results->warnings.insert(results->warnings.end(), vr.warnings.begin(), vr.warnings.end());
        results->errors.insert(results->errors.end(), vr.errors.begin(), vr.errors.end());
    }

    output->append("nIndexes", collection->getIndexCatalog()->numIndexesReady(opCtx));
    output->append("keysPerIndex", keysPerIndex->done());
    if (indexDetails) {
        output->append("indexDetails", indexDetails->done());
    }
}

template <typename T>
void addErrorIfUnequal(T stored, T cached, StringData name, ValidateResults* results) {
    if (stored != cached) {
        results->valid = false;
        results->errors.push_back(str::stream() << "stored value for " << name
                                                << " does not match cached value: " << stored
                                                << " != " << cached);
    }
}

void _validateCatalogEntry(OperationContext* opCtx,
                           Collection* coll,
                           BSONObj validatorDoc,
                           ValidateResults* results) {
    CollectionOptions options = DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, coll->ns());
    if (options.uuid) {
        addErrorIfUnequal(*(options.uuid), coll->uuid(), "UUID", results);
    } else {
        results->valid = false;
        results->errors.push_back("UUID missing on collection.");
    }
    const CollatorInterface* collation = coll->getDefaultCollator();
    addErrorIfUnequal(options.collation.isEmpty(), !collation, "simple collation", results);
    if (!options.collation.isEmpty() && collation)
        addErrorIfUnequal(options.collation.toString(),
                          collation->getSpec().toBSON().toString(),
                          "collation",
                          results);
    addErrorIfUnequal(options.capped, coll->isCapped(), "is capped", results);

    addErrorIfUnequal(options.validator.toString(), validatorDoc.toString(), "validator", results);
    if (!options.validator.isEmpty() && !validatorDoc.isEmpty()) {
        addErrorIfUnequal(options.validationAction.length() ? options.validationAction : "error",
                          coll->getValidationAction().toString(),
                          "validation action",
                          results);
        addErrorIfUnequal(options.validationLevel.length() ? options.validationLevel : "strict",
                          coll->getValidationLevel().toString(),
                          "validation level",
                          results);
    }

    addErrorIfUnequal(options.isView(), false, "is a view", results);
    auto status = options.validateForStorage();
    if (!status.isOK()) {
        results->valid = false;
        results->errors.push_back(str::stream() << "collection options are not valid for storage: "
                                                << options.toBSON());
    }
}

}  // namespace

Status validate(OperationContext* opCtx,
                const NamespaceString& nss,
                ValidateCmdLevel level,
                bool background,
                ValidateResults* results,
                BSONObjBuilder* output) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!(background && (level == kValidateFull)));

    if (background) {
        // Force a checkpoint to ensure background validation has a checkpoint on which to run.
        // TODO (SERVER-43134): to sort out how to do this properly.
        opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx);
    }

    AutoGetDb autoDB(opCtx, nss.db(), MODE_IX);
    boost::optional<Lock::CollectionLock> collLock;
    if (background) {
        collLock.emplace(opCtx, nss, MODE_IX);
    } else {
        collLock.emplace(opCtx, nss, MODE_X);
    }

    Collection* collection = autoDB.getDb() ? autoDB.getDb()->getCollection(opCtx, nss) : nullptr;
    if (!collection) {
        if (autoDB.getDb() && ViewCatalog::get(autoDB.getDb())->lookup(opCtx, nss.ns())) {
            return {ErrorCodes::CommandNotSupportedOnView, "Cannot validate a view"};
        }

        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection '" << nss << "' does not exist to validate."};
    }

    output->append("ns", nss.ns());

    ValidateResultsMap indexNsResultsMap;
    BSONObjBuilder keysPerIndex;  // not using subObjStart to be exception safe.
    IndexConsistency indexConsistency(opCtx, collection);
    ValidateAdaptor indexValidator = ValidateAdaptor(&indexConsistency, level, &indexNsResultsMap);

    try {
        std::map<std::string, int64_t> numIndexKeysPerIndex;

        // Full validation code is executed before we open cursors because it may close
        // and/or invalidate all open cursors.
        if (level == kValidateFull) {
            invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));

            // For full validation we use the storage engine's validation functionality.
            collection->getRecordStore()->validate(opCtx, results, output);

            // For full validation, we validate the internal structure of each index and save the
            // number of keys in the index to compare against _validateIndexes()'s count results.
            numIndexKeysPerIndex = _validateIndexesInternalStructure(
                opCtx, collection->getIndexCatalog(), &indexNsResultsMap, results);
        }

        // We want to share the same data throttle instance across all the cursors used during this
        // validation. Validations started on other collections will not share the same data
        // throttle instance.
        std::shared_ptr<DataThrottle> dataThrottle = std::make_shared<DataThrottle>();

        if (!background) {
            dataThrottle->turnThrottlingOff();
        }

        // Background validation will read from the last stable checkpoint instead of the latest
        // data. This allows concurrent writes to go ahead without interfering with validation's
        // view of the data.
        // The checkpoint lock must be taken around cursor creation to ensure all cursors
        // point at the same checkpoint, i.e. a consistent view of the collection data.
        std::unique_ptr<StorageEngine::CheckpointLock> checkpointCursorsLock;
        if (background) {
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            invariant(storageEngine->supportsCheckpoints());
            opCtx->recoveryUnit()->abandonSnapshot();
            opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kCheckpoint);
            checkpointCursorsLock = storageEngine->getCheckpointLock(opCtx);
        }

        // Open all cursors at once before running non-full validation code so that all steps of
        // validation during background validation use the same view of the data.
        const std::map<std::string, std::unique_ptr<SortedDataInterfaceThrottleCursor>>
            indexCursors = _openIndexCursors(opCtx, collection->getIndexCatalog(), dataThrottle);
        const std::unique_ptr<SeekableRecordThrottleCursor> traverseRecordStoreCursor =
            std::make_unique<SeekableRecordThrottleCursor>(
                opCtx, collection->getRecordStore(), dataThrottle);
        const std::unique_ptr<SeekableRecordThrottleCursor> seekRecordStoreCursor =
            std::make_unique<SeekableRecordThrottleCursor>(
                opCtx, collection->getRecordStore(), dataThrottle);

        checkpointCursorsLock.reset();

        // Because SeekableRecordCursors don't have a method to reset to the start, we save and then
        // use a seek to the first RecordId to reset the cursor (and reuse it) as needed. When
        // iterating through a Record Store cursor, we initialize the loop (and obtain the first
        // Record) with a seek to the first Record (using firstRecordId). Subsequent loop iterations
        // use cursor->next() to get subsequent Records. However, if the Record Store is empty,
        // there is no first record. In this case, we set the first Record Id to an invalid RecordId
        // (RecordId()), which will halt iteration at the initialization step.
        const boost::optional<Record> record = traverseRecordStoreCursor->next(opCtx);
        const RecordId firstRecordId = record ? record->id : RecordId();

        const string uuidString = str::stream() << " (UUID: " << collection->uuid() << ")";

        // Validate the record store.
        log(LogComponent::kIndex) << "validating collection " << collection->ns() << uuidString;
        // In traverseRecordStore(), the index validator keeps track the records in the record
        // store so that _validateIndexes() can confirm that the index entries match the records in
        // the collection.
        indexValidator.traverseRecordStore(opCtx,
                                           collection,
                                           firstRecordId,
                                           traverseRecordStoreCursor,
                                           seekRecordStoreCursor,
                                           background,
                                           results,
                                           output);

        // Validate in-memory catalog information with persisted info.
        _validateCatalogEntry(opCtx, collection, collection->getValidatorDoc(), results);

        // Pause collection validation while a lock is held and between collection and index data
        // valiation.
        //
        // The IndexConsistency object saves document key information during collection data
        // validation and then compares against that key information during index data validation.
        // This fail point is placed in between them, in an attempt to catch any inconsistencies
        // that concurrent CRUD ops might cause if we were to have a bug.
        //
        // Only useful for background validation because we hold an intent lock instead of an
        // exclusive lock, and thus allow concurrent operations.
        if (MONGO_FAIL_POINT(pauseCollectionValidationWithLock)) {
            invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_IX));
            _validationIsPausedForTest.store(true);
            log() << "Failpoint 'pauseCollectionValidationWithLock' activated.";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(pauseCollectionValidationWithLock);
            _validationIsPausedForTest.store(false);
        }

        // Validate indexes and check for mismatches.
        if (results->valid) {
            _validateIndexes(opCtx,
                             collection->getIndexCatalog(),
                             &keysPerIndex,
                             &indexValidator,
                             level,
                             indexCursors,
                             numIndexKeysPerIndex,
                             &indexNsResultsMap,
                             results);

            if (indexConsistency.haveEntryMismatch()) {
                log(LogComponent::kIndex)
                    << "Index inconsistencies were detected on collection " << collection->ns()
                    << ". Starting the second phase of index validation to gather concise errors.";
                _gatherIndexEntryErrors(opCtx,
                                        collection,
                                        &indexConsistency,
                                        &indexValidator,
                                        firstRecordId,
                                        traverseRecordStoreCursor,
                                        seekRecordStoreCursor,
                                        indexCursors,
                                        &indexNsResultsMap,
                                        results);
            }
        }

        // Validate index key count.
        if (results->valid) {
            _validateIndexKeyCount(opCtx, collection, &indexValidator, &indexNsResultsMap);
        }

        // Report the validation results for the user to see.
        _reportValidationResults(
            opCtx, collection, &indexNsResultsMap, &keysPerIndex, level, results, output);

        if (!results->valid) {
            log(LogComponent::kIndex) << "Validation complete for collection " << collection->ns()
                                      << uuidString << ". Corruption found.";
        } else {
            log(LogComponent::kIndex) << "Validation complete for collection " << collection->ns()
                                      << uuidString << ". No corruption found.";
        }
    } catch (DBException& e) {
        if (ErrorCodes::isInterruption(e.code())) {
            return e.toStatus();
        }
        string err = str::stream() << "exception during index validation: " << e.toString();
        results->errors.push_back(err);
        results->valid = false;
    }

    return Status::OK();
}

bool getIsValidationPausedForTest() {
    return _validationIsPausedForTest.load();
}

}  // namespace CollectionValidation
}  // namespace mongo

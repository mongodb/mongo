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
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

using logger::LogComponent;

namespace CollectionValidation {

namespace {

using ValidateResultsMap = std::map<string, ValidateResults>;

/**
 * General validation logic for any RecordStore. Performs sanity checks to confirm that each
 * record in the store is valid according to the given RecordStoreValidateAdaptor and updates
 * record store stats to match.
 */
void _genericRecordStoreValidate(OperationContext* opCtx,
                                 RecordStore* recordStore,
                                 RecordStoreValidateAdaptor* indexValidator,
                                 ValidateResults* results,
                                 BSONObjBuilder* output) {
    long long nrecords = 0;
    long long dataSizeTotal = 0;
    long long nInvalid = 0;

    results->valid = true;
    std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(opCtx, true);
    int interruptInterval = 4096;
    RecordId prevRecordId;

    while (auto record = cursor->next()) {
        if (!(nrecords % interruptInterval)) {
            opCtx->checkForInterrupt();
        }
        ++nrecords;
        auto dataSize = record->data.size();
        dataSizeTotal += dataSize;
        size_t validatedSize;
        Status status = indexValidator->validate(record->id, record->data, &validatedSize);

        // Check to ensure isInRecordIdOrder() is being used properly.
        if (prevRecordId.isValid()) {
            invariant(prevRecordId < record->id);
        }

        // ValidatedSize = dataSize is not a general requirement as some storage engines may use
        // padding, but we still require that they return the unpadded record data.
        if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
            if (results->valid) {
                // Only log once.
                results->errors.push_back("detected one or more invalid documents (see logs)");
            }
            nInvalid++;
            results->valid = false;
            log() << "document at location: " << record->id << " is corrupted";
        }

        prevRecordId = record->id;
    }

    if (results->valid) {
        recordStore->updateStatsAfterRepair(opCtx, nrecords, dataSizeTotal);
    }

    output->append("nInvalidDocuments", nInvalid);
    output->appendNumber("nrecords", nrecords);
}

void _validateRecordStore(OperationContext* opCtx,
                          RecordStore* recordStore,
                          ValidateCmdLevel level,
                          bool background,
                          RecordStoreValidateAdaptor* indexValidator,
                          ValidateResults* results,
                          BSONObjBuilder* output) {
    if (background) {
        indexValidator->traverseRecordStore(recordStore, results, output);
    } else {
        // For 'full' validation we use the record store's validation functionality.
        if (level == kValidateFull) {
            recordStore->validate(opCtx, results, output);
        }
        _genericRecordStoreValidate(opCtx, recordStore, indexValidator, results, output);
    }
}

void _validateIndexes(OperationContext* opCtx,
                      IndexCatalog* indexCatalog,
                      BSONObjBuilder* keysPerIndex,
                      RecordStoreValidateAdaptor* indexValidator,
                      ValidateCmdLevel level,
                      ValidateResultsMap* indexNsResultsMap,
                      ValidateResults* results) {

    std::unique_ptr<IndexCatalog::IndexIterator> it = indexCatalog->getIndexIterator(opCtx, false);

    // Validate Indexes.
    while (it->more()) {
        opCtx->checkForInterrupt();
        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();

        log(LogComponent::kIndex) << "validating index " << descriptor->indexName()
                                  << " on collection " << descriptor->parentNS();
        ValidateResults& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];
        bool checkCounts = false;
        int64_t numTraversedKeys;
        int64_t numValidatedKeys;

        if (level == kValidateFull) {
            iam->validate(opCtx, &numValidatedKeys, &curIndexResults);
            checkCounts = true;
        }

        if (curIndexResults.valid) {
            indexValidator->traverseIndex(iam, descriptor, &curIndexResults, &numTraversedKeys);

            if (checkCounts && (numValidatedKeys != numTraversedKeys)) {
                curIndexResults.valid = false;
                string msg = str::stream()
                    << "number of traversed index entries (" << numTraversedKeys
                    << ") does not match the number of expected index entries (" << numValidatedKeys
                    << ")";
                results->errors.push_back(msg);
                results->valid = false;
            }

            if (curIndexResults.valid) {
                keysPerIndex->appendNumber(descriptor->indexName(),
                                           static_cast<long long>(numTraversedKeys));
            } else {
                results->valid = false;
            }
        } else {
            results->valid = false;
        }
    }
}

/**
 * Executes the second phase of validation for improved error reporting. This is only done if
 * any index inconsistencies are found during the first phase of validation.
 */
void _gatherIndexEntryErrors(OperationContext* opCtx,
                             RecordStore* recordStore,
                             IndexCatalog* indexCatalog,
                             IndexConsistency* indexConsistency,
                             RecordStoreValidateAdaptor* indexValidator,
                             ValidateResultsMap* indexNsResultsMap,
                             ValidateResults* result) {
    indexConsistency->setSecondPhase();

    log(LogComponent::kIndex) << "Starting to traverse through all the document key sets.";

    // During the second phase of validation, iterate through each documents key set and only record
    // the keys that were inconsistent during the first phase of validation.
    std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(opCtx, true);
    while (auto record = cursor->next()) {
        opCtx->checkForInterrupt();

        // We can ignore the status of validate as it was already checked during the first phase.
        size_t validatedSize;
        indexValidator->validate(record->id, record->data, &validatedSize).ignore();
    }

    log(LogComponent::kIndex) << "Finished traversing through all the document key sets.";
    log(LogComponent::kIndex) << "Starting to traverse through all the indexes.";

    // Iterate through all the indexes in the collection and only record the index entry keys that
    // had inconsistencies during the first phase.
    std::unique_ptr<IndexCatalog::IndexIterator> it = indexCatalog->getIndexIterator(opCtx, false);
    while (it->more()) {
        opCtx->checkForInterrupt();

        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();

        log(LogComponent::kIndex) << "Traversing through the index entries for index "
                                  << descriptor->indexName() << ".";
        indexValidator->traverseIndex(
            iam, descriptor, /*ValidateResults=*/nullptr, /*numTraversedKeys=*/nullptr);
    }

    log(LogComponent::kIndex) << "Finished traversing through all the indexes.";

    indexConsistency->addIndexEntryErrors(indexNsResultsMap, result);
}

void _validateIndexKeyCount(OperationContext* opCtx,
                            IndexCatalog* indexCatalog,
                            RecordStore* recordStore,
                            RecordStoreValidateAdaptor* indexValidator,
                            ValidateResultsMap* indexNsResultsMap) {

    std::unique_ptr<IndexCatalog::IndexIterator> indexIterator =
        indexCatalog->getIndexIterator(opCtx, false);
    while (indexIterator->more()) {
        const IndexDescriptor* descriptor = indexIterator->next()->descriptor();
        ValidateResults& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];

        if (curIndexResults.valid) {
            indexValidator->validateIndexKeyCount(
                descriptor, recordStore->numRecords(opCtx), curIndexResults);
        }
    }
}

void _reportValidationResults(OperationContext* opCtx,
                              IndexCatalog* indexCatalog,
                              ValidateResultsMap* indexNsResultsMap,
                              BSONObjBuilder* keysPerIndex,
                              ValidateCmdLevel level,
                              ValidateResults* results,
                              BSONObjBuilder* output) {

    std::unique_ptr<BSONObjBuilder> indexDetails;
    if (level == kValidateFull) {
        indexDetails = std::make_unique<BSONObjBuilder>();
    }

    // Report index validation results.
    for (const auto& it : *indexNsResultsMap) {
        const string indexNs = it.first;
        const ValidateResults& vr = it.second;

        if (!vr.valid) {
            results->valid = false;
        }

        if (indexDetails) {
            BSONObjBuilder bob(indexDetails->subobjStart(indexNs));
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

    output->append("nIndexes", indexCatalog->numIndexesReady(opCtx));
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
                                                << " does not match cached value: "
                                                << stored
                                                << " != "
                                                << cached);
    }
}

void _validateCatalogEntry(OperationContext* opCtx,
                           Collection* coll,
                           BSONObj validatorDoc,
                           ValidateResults* results) {
    CollectionOptions options = DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, coll->ns());
    invariant(options.uuid);
    addErrorIfUnequal(*(options.uuid), coll->uuid(), "UUID", results);
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
                Collection* coll,
                ValidateCmdLevel level,
                bool background,
                ValidateResults* results,
                BSONObjBuilder* output) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(coll->ns(), MODE_IS));

    try {
        ValidateResultsMap indexNsResultsMap;
        BSONObjBuilder keysPerIndex;  // not using subObjStart to be exception safe.
        IndexConsistency indexConsistency(
            opCtx, coll, coll->ns(), coll->getRecordStore(), background);
        RecordStoreValidateAdaptor indexValidator = RecordStoreValidateAdaptor(
            opCtx, &indexConsistency, level, coll->getIndexCatalog(), &indexNsResultsMap);

        string uuidString = str::stream() << " (UUID: " << coll->uuid() << ")";

        // Validate the record store.
        log(LogComponent::kIndex) << "validating collection " << coll->ns() << uuidString;
        _validateRecordStore(
            opCtx, coll->getRecordStore(), level, background, &indexValidator, results, output);

        // Validate in-memory catalog information with the persisted info.
        _validateCatalogEntry(opCtx, coll, coll->getValidatorDoc(), results);

        // Validate indexes and check for mismatches.
        if (results->valid) {
            _validateIndexes(opCtx,
                             coll->getIndexCatalog(),
                             &keysPerIndex,
                             &indexValidator,
                             level,
                             &indexNsResultsMap,
                             results);

            if (indexConsistency.haveEntryMismatch()) {
                log(LogComponent::kIndex)
                    << "Index inconsistencies were detected on collection " << coll->ns()
                    << ". Starting the second phase of index validation to gather concise errors.";
                _gatherIndexEntryErrors(opCtx,
                                        coll->getRecordStore(),
                                        coll->getIndexCatalog(),
                                        &indexConsistency,
                                        &indexValidator,
                                        &indexNsResultsMap,
                                        results);
            }
        }

        // Validate index key count.
        if (results->valid) {
            _validateIndexKeyCount(opCtx,
                                   coll->getIndexCatalog(),
                                   coll->getRecordStore(),
                                   &indexValidator,
                                   &indexNsResultsMap);
        }

        // Report the validation results for the user to see.
        _reportValidationResults(opCtx,
                                 coll->getIndexCatalog(),
                                 &indexNsResultsMap,
                                 &keysPerIndex,
                                 level,
                                 results,
                                 output);

        if (!results->valid) {
            log(LogComponent::kIndex) << "Validation complete for collection " << coll->ns()
                                      << uuidString << ". Corruption found.";
        } else {
            log(LogComponent::kIndex) << "Validation complete for collection " << coll->ns()
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
}  // namespace CollectionValidation
}  // namespace mongo

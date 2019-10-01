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

#include <fmt/format.h>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/validate_adaptor.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_catalog.h"
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
 * Validates the internal structure of each index in the Index Catalog 'indexCatalog', ensuring that
 * the index files have not been corrupted or compromised.
 *
 * May close or invalidate open cursors.
 *
 * Returns a map from indexName -> number of keys validated.
 */
std::map<std::string, int64_t> _validateIndexesInternalStructure(
    OperationContext* opCtx,
    ValidateState* validateState,
    ValidateResultsMap* indexNsResultsMap,
    ValidateResults* results) {
    std::map<std::string, int64_t> numIndexKeysPerIndex;
    // Need to use the IndexCatalog here because the 'validateState->indexes' object hasn't been
    // constructed yet.
    const IndexCatalog* indexCatalog = validateState->getCollection()->getIndexCatalog();
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
void _validateIndexes(OperationContext* opCtx,
                      ValidateState* validateState,
                      BSONObjBuilder* keysPerIndex,
                      ValidateAdaptor* indexValidator,
                      const std::map<std::string, int64_t>& numIndexKeysPerIndex,
                      ValidateResultsMap* indexNsResultsMap,
                      ValidateResults* results) {
    // Validate Indexes, checking for mismatch between index entries and collection records.
    for (const auto& index : validateState->getIndexes()) {
        opCtx->checkForInterrupt();

        const IndexDescriptor* descriptor = index->descriptor();

        log(LogComponent::kIndex) << "validating index consistency " << descriptor->indexName()
                                  << " on collection " << descriptor->parentNS();

        ValidateResults& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];
        int64_t numTraversedKeys;
        indexValidator->traverseIndex(opCtx, index.get(), &numTraversedKeys, &curIndexResults);

        // If we are performing a full validation, we have information on the number of index keys
        // validated in _validateIndexesInternalStructure (when we validated the internal structure
        // of the index). Check if this is consistent with 'numTraversedKeys' from traverseIndex
        // above.
        if (validateState->isFullValidate()) {
            invariant(opCtx->lockState()->isCollectionLockedForMode(validateState->nss(), MODE_X));

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
void _gatherIndexEntryErrors(OperationContext* opCtx,
                             ValidateState* validateState,
                             IndexConsistency* indexConsistency,
                             ValidateAdaptor* indexValidator,
                             ValidateResultsMap* indexNsResultsMap,
                             ValidateResults* result) {
    indexConsistency->setSecondPhase();

    log(LogComponent::kIndex) << "Starting to traverse through all the document key sets.";

    // During the second phase of validation, iterate through each documents key set and only record
    // the keys that were inconsistent during the first phase of validation.
    {
        ValidateResults tempValidateResults;
        BSONObjBuilder tempBuilder;
        indexValidator->traverseRecordStore(opCtx, &tempValidateResults, &tempBuilder);
    }

    log(LogComponent::kIndex) << "Finished traversing through all the document key sets.";
    log(LogComponent::kIndex) << "Starting to traverse through all the indexes.";

    // Iterate through all the indexes in the collection and only record the index entry keys that
    // had inconsistencies during the first phase.
    for (const auto& index : validateState->getIndexes()) {
        opCtx->checkForInterrupt();

        const IndexDescriptor* descriptor = index->descriptor();

        log(LogComponent::kIndex) << "Traversing through the index entries for index "
                                  << descriptor->indexName() << ".";

        indexValidator->traverseIndex(opCtx,
                                      index.get(),
                                      /*numTraversedKeys=*/nullptr,
                                      /*ValidateResults=*/nullptr);
    }

    log(LogComponent::kIndex) << "Finished traversing through all the indexes.";

    indexConsistency->addIndexEntryErrors(indexNsResultsMap, result);
}

void _validateIndexKeyCount(OperationContext* opCtx,
                            ValidateState* validateState,
                            ValidateAdaptor* indexValidator,
                            ValidateResultsMap* indexNsResultsMap) {
    for (const auto& index : validateState->getIndexes()) {
        const IndexDescriptor* descriptor = index->descriptor();
        ValidateResults& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];

        if (curIndexResults.valid) {
            indexValidator->validateIndexKeyCount(
                descriptor,
                validateState->getCollection()->getRecordStore()->numRecords(opCtx),
                curIndexResults);
        }
    }
}

void _reportValidationResults(OperationContext* opCtx,
                              ValidateState* validateState,
                              ValidateResultsMap* indexNsResultsMap,
                              BSONObjBuilder* keysPerIndex,
                              ValidateResults* results,
                              BSONObjBuilder* output) {
    std::unique_ptr<BSONObjBuilder> indexDetails;

    if (validateState->isFullValidate()) {
        invariant(opCtx->lockState()->isCollectionLockedForMode(validateState->nss(), MODE_X));
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

    output->append("nIndexes", static_cast<int>(validateState->getIndexes().size()));
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

std::string multikeyPathsToString(MultikeyPaths paths) {
    str::stream builder;
    builder << "[";
    auto pathIt = paths.begin();
    while (true) {
        builder << "{";

        auto pathSet = *pathIt;
        auto setIt = pathSet.begin();
        while (true) {
            builder << *setIt++;
            if (setIt == pathSet.end()) {
                break;
            } else {
                builder << ",";
            }
        }
        builder << "}";

        if (++pathIt == paths.end()) {
            break;
        } else {
            builder << ",";
        }
    }
    builder << "]";
    return builder;
}

void _validateCatalogEntry(OperationContext* opCtx,
                           ValidateState* validateState,
                           ValidateResults* results) {
    Collection* collection = validateState->getCollection();
    CollectionOptions options =
        DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, validateState->nss());
    if (options.uuid) {
        addErrorIfUnequal(*(options.uuid), validateState->uuid(), "UUID", results);
    } else {
        results->valid = false;
        results->errors.push_back("UUID missing on collection.");
    }
    const CollatorInterface* collation = collection->getDefaultCollator();
    addErrorIfUnequal(options.collation.isEmpty(), !collation, "simple collation", results);
    if (!options.collation.isEmpty() && collation)
        addErrorIfUnequal(options.collation.toString(),
                          collation->getSpec().toBSON().toString(),
                          "collation",
                          results);
    addErrorIfUnequal(options.capped, collection->isCapped(), "is capped", results);

    BSONObj validatorDoc = collection->getValidatorDoc();
    addErrorIfUnequal(options.validator.toString(), validatorDoc.toString(), "validator", results);
    if (!options.validator.isEmpty() && !validatorDoc.isEmpty()) {
        addErrorIfUnequal(options.validationAction.length() ? options.validationAction : "error",
                          collection->getValidationAction().toString(),
                          "validation action",
                          results);
        addErrorIfUnequal(options.validationLevel.length() ? options.validationLevel : "strict",
                          collection->getValidationLevel().toString(),
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

    std::vector<std::string> indexes;
    DurableCatalog::get(opCtx)->getReadyIndexes(opCtx, validateState->nss(), &indexes);
    for (auto& index : indexes) {
        MultikeyPaths multikeyPaths;
        const bool isMultikey = DurableCatalog::get(opCtx)->isIndexMultikey(
            opCtx, validateState->nss(), index, &multikeyPaths);
        const bool hasMultiKeyPaths = std::any_of(multikeyPaths.begin(),
                                                  multikeyPaths.end(),
                                                  [](auto& pathSet) { return pathSet.size() > 0; });
        // It is illegal for multikey paths to exist without the multikey flag set on the index, but
        // it may be possible for multikey to be set on the index while having no multikey paths. If
        // any of the paths are multikey, then the entire index should also be marked multikey.
        if (hasMultiKeyPaths && !isMultikey) {
            results->valid = false;
            results->errors.push_back(fmt::format(
                "The 'multikey' field for index {} was false with non-empty 'multikeyPaths': {}",
                index,
                multikeyPathsToString(multikeyPaths)));
        }
    }
}
}  // namespace

Status validate(OperationContext* opCtx,
                const NamespaceString& nss,
                const bool fullValidate,
                bool background,
                ValidateResults* results,
                BSONObjBuilder* output) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!(background && fullValidate));

    // This is deliberately outside of the try-catch block, so that any errors thrown in the
    // constructor fail the cmd, as opposed to returning OK with valid:false.
    ValidateState validateState(opCtx, nss, background, fullValidate);

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    // Check whether we are allowed to read from this node after acquiring our locks. If we are
    // in a state where we cannot read, we should not run validate.
    uassertStatusOK(replCoord->checkCanServeReadsFor(
        opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

    try {
        std::map<std::string, int64_t> numIndexKeysPerIndex;
        ValidateResultsMap indexNsResultsMap;
        BSONObjBuilder keysPerIndex;  // not using subObjStart to be exception safe.

        // Full validation code is executed before we open cursors because it may close
        // and/or invalidate all open cursors.
        if (fullValidate) {
            invariant(opCtx->lockState()->isCollectionLockedForMode(validateState.nss(), MODE_X));

            // For full validation we use the storage engine's validation functionality.
            validateState.getCollection()->getRecordStore()->validate(opCtx, results, output);
            // For full validation, we validate the internal structure of each index and save the
            // number of keys in the index to compare against _validateIndexes()'s count results.
            numIndexKeysPerIndex = _validateIndexesInternalStructure(
                opCtx, &validateState, &indexNsResultsMap, results);
        }

        // Validate in-memory catalog information with persisted info prior to setting the read
        // source to kCheckpoint otherwise we'd use a checkpointed MDB catalog file.
        _validateCatalogEntry(opCtx, &validateState, results);

        // Open all cursors at once before running non-full validation code so that all steps of
        // validation during background validation use the same view of the data.
        validateState.initializeCursors(opCtx);

        const string uuidString = str::stream() << " (UUID: " << validateState.uuid() << ")";

        // Validate the record store.
        log(LogComponent::kIndex) << "validating collection " << validateState.nss() << uuidString;

        IndexConsistency indexConsistency(opCtx, &validateState);
        ValidateAdaptor indexValidator(&indexConsistency, &validateState, &indexNsResultsMap);

        // In traverseRecordStore(), the index validator keeps track the records in the record
        // store so that _validateIndexes() can confirm that the index entries match the records in
        // the collection.
        indexValidator.traverseRecordStore(opCtx, results, output);

        // Pause collection validation while a lock is held and between collection and index data
        // validation.
        //
        // The IndexConsistency object saves document key information during collection data
        // validation and then compares against that key information during index data validation.
        // This fail point is placed in between them, in an attempt to catch any inconsistencies
        // that concurrent CRUD ops might cause if we were to have a bug.
        //
        // Only useful for background validation because we hold an intent lock instead of an
        // exclusive lock, and thus allow concurrent operations.

        if (MONGO_unlikely(pauseCollectionValidationWithLock.shouldFail())) {
            _validationIsPausedForTest.store(true);
            log() << "Failpoint 'pauseCollectionValidationWithLock' activated.";
            pauseCollectionValidationWithLock.pauseWhileSet();
            _validationIsPausedForTest.store(false);
        }

        // Validate indexes and check for mismatches.
        if (results->valid) {
            _validateIndexes(opCtx,
                             &validateState,
                             &keysPerIndex,
                             &indexValidator,
                             numIndexKeysPerIndex,
                             &indexNsResultsMap,
                             results);

            if (indexConsistency.haveEntryMismatch()) {
                log(LogComponent::kIndex)
                    << "Index inconsistencies were detected on collection " << validateState.nss()
                    << ". Starting the second phase of index validation to gather concise errors.";
                _gatherIndexEntryErrors(opCtx,
                                        &validateState,
                                        &indexConsistency,
                                        &indexValidator,
                                        &indexNsResultsMap,
                                        results);
            }
        }

        // Validate index key count.
        if (results->valid) {
            _validateIndexKeyCount(opCtx, &validateState, &indexValidator, &indexNsResultsMap);
        }

        // Report the validation results for the user to see.
        _reportValidationResults(
            opCtx, &validateState, &indexNsResultsMap, &keysPerIndex, results, output);

        if (!results->valid) {
            log(LogComponent::kIndex) << "Validation complete for collection "
                                      << validateState.nss() << uuidString << ". Corruption found.";
        } else {
            log(LogComponent::kIndex)
                << "Validation complete for collection " << validateState.nss() << uuidString
                << ". No corruption found.";
        }

        output->append("ns", validateState.nss().ns());
    } catch (ExceptionFor<ErrorCodes::CursorNotFound>&) {
        invariant(background);
        string warning = str::stream()
            << "Collection validation with {background: true} validates"
            << " the latest checkpoint (data in a snapshot written to disk in a consistent"
            << " way across all data files). During this validation, some tables have not yet been"
            << " checkpointed.";
        results->warnings.push_back(warning);

        // Nothing to validate, so it must be valid.
        results->valid = true;
        return Status::OK();
    } catch (DBException& e) {
        if (ErrorCodes::isInterruption(e.code())) {
            return e.toStatus();
        }
        string err = str::stream() << "exception during collection validation: " << e.toString();
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

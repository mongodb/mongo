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


#include <algorithm>
#include <boost/optional.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_options_gen.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/validate_adaptor.h"
#include "mongo/db/catalog/validate_state.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

using logv2::LogComponent;
using std::string;

MONGO_FAIL_POINT_DEFINE(pauseCollectionValidationWithLock);

namespace CollectionValidation {

namespace {

// Indicates whether the failpoint turned on by testing has been reached.
AtomicWord<bool> _validationIsPausedForTest{false};

/**
 * Validates the internal structure of each index in the Index Catalog 'indexCatalog', ensuring that
 * the index files have not been corrupted or compromised.
 *
 * May close or invalidate open cursors.
 */
void _validateIndexesInternalStructure(OperationContext* opCtx,
                                       bool full,
                                       ValidateState* validateState,
                                       ValidateResults* results) {
    // Need to use the IndexCatalog here because the 'validateState->indexes' object hasn't been
    // constructed yet. It must be initialized to ensure we're validating all indexes.
    const IndexCatalog* indexCatalog = validateState->getCollection()->getIndexCatalog();
    const auto it = indexCatalog->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);

    // Validate Indexes Internal Structure, checking if index files have been compromised or
    // corrupted.
    while (it->more()) {
        opCtx->checkForInterrupt();

        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();

        LOGV2_OPTIONS(20295,
                      {LogComponent::kIndex},
                      "Validating internal structure",
                      "index"_attr = descriptor->indexName(),
                      logAttrs(validateState->nss()));

        auto indexResults = iam->validate(opCtx, full);

        if (!indexResults.valid) {
            results->valid = false;
        }

        results->indexResultsMap[descriptor->indexName()] = std::move(indexResults);
    }
}

/**
 * Validates each index in the Index Catalog using the cursors in 'indexCursors'.
 *
 * If 'level' is kValidateFull, then we will compare new index entry counts with a previously taken
 * count saved in 'numIndexKeysPerIndex'.
 */
void _validateIndexes(OperationContext* opCtx,
                      ValidateState* validateState,
                      ValidateAdaptor* indexValidator,
                      ValidateResults* results) {
    // Validate Indexes, checking for mismatch between index entries and collection records.
    for (const auto& indexIdent : validateState->getIndexIdents()) {
        opCtx->checkForInterrupt();

        // Make a copy of the index name. The `traverseIndex()` function below will yield
        // periodically, so it's unsafe to hold a pointer to the index here.
        const std::string indexName = validateState->getCollection()
                                          ->getIndexCatalog()
                                          ->findIndexByIdent(opCtx, indexIdent)
                                          ->indexName();

        LOGV2_OPTIONS(20296,
                      {LogComponent::kIndex},
                      "Validating index consistency",
                      "index"_attr = indexName,
                      logAttrs(validateState->nss()));

        int64_t numTraversedKeys;
        indexValidator->traverseIndex(opCtx,
                                      validateState->getCollection()
                                          ->getIndexCatalog()
                                          ->findIndexByIdent(opCtx, indexIdent)
                                          ->getEntry(),
                                      &numTraversedKeys,
                                      results);

        auto& curIndexResults = (results->indexResultsMap)[indexName];
        curIndexResults.keysTraversed = numTraversedKeys;

        if (!curIndexResults.valid) {
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
                             ValidateAdaptor* indexValidator,
                             ValidateResults* result) {
    indexValidator->setSecondPhase();
    if (!indexValidator->limitMemoryUsageForSecondPhase(result)) {
        return;
    }

    LOGV2_OPTIONS(
        20297, {LogComponent::kIndex}, "Starting to traverse through all the document key sets");

    // During the second phase of validation, iterate through each documents key set and only record
    // the keys that were inconsistent during the first phase of validation.
    {
        ValidateResults tempValidateResults;
        BSONObjBuilder tempBuilder;

        indexValidator->traverseRecordStore(
            opCtx, &tempValidateResults, &tempBuilder, validateState->validationVersion());
    }

    LOGV2_OPTIONS(
        20298, {LogComponent::kIndex}, "Finished traversing through all the document key sets");
    LOGV2_OPTIONS(20299, {LogComponent::kIndex}, "Starting to traverse through all the indexes");

    // Iterate through all the indexes in the collection and only record the index entry keys that
    // had inconsistencies during the first phase.
    for (const auto& indexIdent : validateState->getIndexIdents()) {
        opCtx->checkForInterrupt();

        const IndexDescriptor* descriptor =
            validateState->getCollection()->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);

        LOGV2_OPTIONS(20300,
                      {LogComponent::kIndex},
                      "Traversing through the index entries",
                      "index"_attr = descriptor->indexName());

        indexValidator->traverseIndex(opCtx,
                                      descriptor->getEntry(),
                                      /*numTraversedKeys=*/nullptr,
                                      result);
    }

    if (result->numRemovedExtraIndexEntries > 0) {
        result->warnings.push_back(str::stream()
                                   << "Removed " << result->numRemovedExtraIndexEntries
                                   << " extra index entries.");
    }

    if (validateState->fixErrors()) {
        indexValidator->repairIndexEntries(opCtx, result);
    }

    LOGV2_OPTIONS(20301, {LogComponent::kIndex}, "Finished traversing through all the indexes");

    indexValidator->addIndexEntryErrors(opCtx, result);
}

void _validateIndexKeyCount(OperationContext* opCtx,
                            ValidateState* validateState,
                            ValidateAdaptor* indexValidator,
                            ValidateResultsMap* indexNsResultsMap) {
    for (const auto& indexIdent : validateState->getIndexIdents()) {
        const IndexDescriptor* descriptor =
            validateState->getCollection()->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
        auto& curIndexResults = (*indexNsResultsMap)[descriptor->indexName()];

        if (curIndexResults.valid) {
            indexValidator->validateIndexKeyCount(opCtx, descriptor->getEntry(), curIndexResults);
        }
    }
}

void _printIndexSpec(OperationContext* opCtx,
                     const ValidateState* validateState,
                     StringData indexName) {
    const IndexDescriptor* descriptor =
        validateState->getCollection()->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto indexSpec = descriptor->infoObj();
    LOGV2_ERROR(7463100, "Index failed validation", "spec"_attr = indexSpec);
}

/**
 * Logs oplog entries related to corrupted records/indexes in validation results.
 */
void _logOplogEntriesForInvalidResults(OperationContext* opCtx, ValidateResults* results) {
    if (results->recordTimestamps.empty()) {
        return;
    }

    LOGV2(
        7464200,
        "Validation failed: oplog timestamps referenced by corrupted collection and index entries",
        "numTimestamps"_attr = results->recordTimestamps.size());

    // Set up read on oplog collection.
    try {
        AutoGetOplog oplogRead(opCtx, OplogAccessMode::kRead);
        const auto& oplogCollection = oplogRead.getCollection();

        if (!oplogCollection) {
            for (auto it = results->recordTimestamps.rbegin();
                 it != results->recordTimestamps.rend();
                 it++) {
                const auto& timestamp = *it;
                LOGV2(8080900,
                      "    Validation failed: Oplog entry timestamp for corrupted collection and "
                      "index entry",
                      "timestamp"_attr = timestamp);
            }
            return;
        }

        // Log oplog entries in reverse from most recent timestamp to oldest.
        // Due to oplog truncation, if we fail to find any oplog entry for a particular timestamp,
        // we can stop searching for oplog entries with earlier timestamps.
        auto recordStore = oplogCollection->getRecordStore();
        uassert(ErrorCodes::InternalError,
                "Validation failed: Unable to get oplog record store for corrupted collection and "
                "index entries",
                recordStore);

        auto cursor = recordStore->getCursor(opCtx, /*forward=*/false);
        uassert(ErrorCodes::CursorNotFound,
                "Validation failed: Unable to get cursor to oplog collection.",
                cursor);

        for (auto it = results->recordTimestamps.rbegin(); it != results->recordTimestamps.rend();
             it++) {
            const auto& timestamp = *it;

            // A record id in the oplog collection is equivalent to the document's timestamp field.
            RecordId recordId(timestamp.asULL());
            auto record = cursor->seekExact(recordId);
            if (!record) {
                LOGV2(7464201,
                      "    Validation failed: Stopping oplog entry search for corrupted collection "
                      "and index entries.",
                      "timestamp"_attr = timestamp);
                break;
            }

            LOGV2(
                7464202,
                "    Validation failed: Oplog entry found for corrupted collection and index entry",
                "timestamp"_attr = timestamp,
                "oplogEntryDoc"_attr = redact(record->data.toBson()));
        }
    } catch (DBException& ex) {
        LOGV2_ERROR(7464203,
                    "Validation failed: Unable to fetch entries from oplog collection for "
                    "corrupted collection and index entries",
                    "ex"_attr = ex);
    }
}

void _reportValidationResults(OperationContext* opCtx,
                              ValidateState* validateState,
                              ValidateResults* results,
                              BSONObjBuilder* output) {
    BSONObjBuilder indexDetails;

    results->readTimestamp = validateState->getValidateTimestamp();

    if (validateState->isFullIndexValidation()) {
        invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
            validateState->nss(), MODE_X));
    }

    BSONObjBuilder keysPerIndex;

    // Report detailed index validation results gathered when using {full: true} for validated
    // indexes.
    int nIndexes = results->indexResultsMap.size();
    for (const auto& [indexName, vr] : results->indexResultsMap) {
        if (!vr.valid) {
            results->valid = false;
            _printIndexSpec(opCtx, validateState, indexName);
        }

        BSONObjBuilder bob(indexDetails.subobjStart(indexName));
        bob.appendBool("valid", vr.valid);

        if (!vr.warnings.empty()) {
            bob.append("warnings", vr.warnings);
        }

        if (!vr.errors.empty()) {
            bob.append("errors", vr.errors);
        }


        keysPerIndex.appendNumber(indexName, static_cast<long long>(vr.keysTraversed));

        results->warnings.insert(results->warnings.end(), vr.warnings.begin(), vr.warnings.end());
        results->errors.insert(results->errors.end(), vr.errors.begin(), vr.errors.end());
    }

    output->append("nIndexes", nIndexes);
    output->append("keysPerIndex", keysPerIndex.done());
    output->append("indexDetails", indexDetails.done());
}

void _reportInvalidResults(OperationContext* opCtx,
                           ValidateState* validateState,
                           ValidateResults* results,
                           BSONObjBuilder* output) {
    _reportValidationResults(opCtx, validateState, results, output);
    _logOplogEntriesForInvalidResults(opCtx, results);
    LOGV2_OPTIONS(20302,
                  {LogComponent::kIndex},
                  "Validation complete -- Corruption found",
                  logAttrs(validateState->nss()),
                  logAttrs(validateState->uuid()));
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

void addErrorIfUnequal(boost::optional<ValidationLevelEnum> stored,
                       boost::optional<ValidationLevelEnum> cached,
                       StringData name,
                       ValidateResults* results) {
    addErrorIfUnequal(ValidationLevel_serializer(validationLevelOrDefault(stored)),
                      ValidationLevel_serializer(validationLevelOrDefault(cached)),
                      name,
                      results);
}

void addErrorIfUnequal(boost::optional<ValidationActionEnum> stored,
                       boost::optional<ValidationActionEnum> cached,
                       StringData name,
                       ValidateResults* results) {
    addErrorIfUnequal(ValidationAction_serializer(validationActionOrDefault(stored)),
                      ValidationAction_serializer(validationActionOrDefault(cached)),
                      name,
                      results);
}

void _validateCatalogEntry(OperationContext* opCtx,
                           ValidateState* validateState,
                           ValidateResults* results) {
    const auto& collection = validateState->getCollection();
    const auto& options = collection->getCollectionOptions();
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
        addErrorIfUnequal(options.validationAction,
                          collection->getValidationAction(),
                          "validation action",
                          results);
        addErrorIfUnequal(
            options.validationLevel, collection->getValidationLevel(), "validation level", results);
    }

    addErrorIfUnequal(options.isView(), false, "is a view", results);
    auto status = options.validateForStorage();
    if (!status.isOK()) {
        results->valid = false;
        results->errors.push_back(str::stream() << "collection options are not valid for storage: "
                                                << options.toBSON());
    }

    const auto& indexCatalog = collection->getIndexCatalog();
    auto indexIt = indexCatalog->getIndexIterator(opCtx,
                                                  IndexCatalog::InclusionPolicy::kReady |
                                                      IndexCatalog::InclusionPolicy::kUnfinished |
                                                      IndexCatalog::InclusionPolicy::kFrozen);

    while (indexIt->more()) {
        const IndexCatalogEntry* indexEntry = indexIt->next();
        const std::string indexName = indexEntry->descriptor()->indexName();

        Status status =
            index_key_validate::validateIndexSpec(opCtx, indexEntry->descriptor()->infoObj())
                .getStatus();
        if (!status.isOK()) {
            results->valid = false;
            results->errors.push_back(
                fmt::format("The index specification for index '{}' contains invalid fields. {}. "
                            "Run the 'collMod' command on the collection without any arguments "
                            "to fix the invalid index options",
                            indexName,
                            status.reason()));
        }

        if (!indexEntry->isReady()) {
            continue;
        }

        MultikeyPaths multikeyPaths;
        const bool isMultikey = collection->isIndexMultikey(opCtx, indexName, &multikeyPaths);
        const bool hasMultiKeyPaths = std::any_of(multikeyPaths.begin(),
                                                  multikeyPaths.end(),
                                                  [](auto& pathSet) { return pathSet.size() > 0; });
        // It is illegal for multikey paths to exist without the multikey flag set on the index,
        // but it may be possible for multikey to be set on the index while having no multikey
        // paths. If any of the paths are multikey, then the entire index should also be marked
        // multikey.
        if (hasMultiKeyPaths && !isMultikey) {
            results->valid = false;
            results->errors.push_back(
                fmt::format("The 'multikey' field for index {} was false with non-empty "
                            "'multikeyPaths': {}",
                            indexName,
                            multikeyPathsToString(multikeyPaths)));
        }
    }
}
}  // namespace

Status validate(OperationContext* opCtx,
                const NamespaceString& nss,
                ValidateMode mode,
                RepairMode repairMode,
                const AdditionalOptions& additionalOptions,
                ValidateResults* results,
                BSONObjBuilder* output,
                bool logDiagnostics,
                const SerializationContext& sc) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked() || storageGlobalParams.repair);

    // This is deliberately outside of the try-catch block, so that any errors thrown in the
    // constructor fail the cmd, as opposed to returning OK with valid:false.
    ValidateState validateState(opCtx, nss, mode, repairMode, additionalOptions, logDiagnostics);

    // Foreground validation needs to ignore prepare conflicts, or else it would deadlock.
    // Repair mode cannot use ignore-prepare because it needs to be able to do writes, and there is
    // no danger of deadlock for this mode anyway since it is only used at startup (or in standalone
    // mode where prepared transactions are prohibited.)
    auto oldPrepareConflictBehavior =
        shard_role_details::getRecoveryUnit(opCtx)->getPrepareConflictBehavior();
    ON_BLOCK_EXIT([&] {
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
        shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
            oldPrepareConflictBehavior);
    });

    // Relax corruption detection so that we log and continue scanning instead of failing early.
    auto oldDataCorruptionMode =
        shard_role_details::getRecoveryUnit(opCtx)->getDataCorruptionDetectionMode();
    shard_role_details::getRecoveryUnit(opCtx)->setDataCorruptionDetectionMode(
        DataCorruptionDetectionMode::kLogAndContinue);
    ON_BLOCK_EXIT([&] {
        shard_role_details::getRecoveryUnit(opCtx)->setDataCorruptionDetectionMode(
            oldDataCorruptionMode);
    });

    if (validateState.fixErrors()) {
        // Note: cannot set PrepareConflictBehavior here, since the validate command with repair
        // needs kIngnoreConflictsAllowWrites, but validate repair at startup cannot set that here
        // due to an already active WriteUnitOfWork.  The prepare conflict behavior for validate
        // command with repair is set in the command code prior to this point.
        invariant(!validateState.isBackground());
    } else if (!validateState.isBackground()) {
        // Foreground validation may perform writes to fix up inconsistencies that are not
        // correctness errors.
        shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
    } else {
        // isBackground().
        invariant(oldPrepareConflictBehavior == PrepareConflictBehavior::kEnforce);
    }

    // TODO SERVER-86914: Enable pre-fetching.
    if (gFeatureFlagPrefetch.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        shard_role_details::getRecoveryUnit(opCtx)->setPrefetching(false);
    }

    Status status = validateState.initializeCollection(opCtx);
    uassertStatusOK(status);

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    // Check whether we are allowed to read from this node after acquiring our locks. If we are
    // in a state where we cannot read, we should not run validate.
    uassertStatusOK(replCoord->checkCanServeReadsFor(
        opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

    output->append("ns", NamespaceStringUtil::serialize(validateState.nss(), sc));

    validateState.uuid().appendToBuilder(output, "uuid");

    try {
        invariant(!validateState.isFullIndexValidation() ||
                  shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                      validateState.nss(), MODE_X));

        // Record store validation code is executed before we open cursors because it may close
        // and/or invalidate all open cursors.
        validateState.getCollection()->getRecordStore()->validate(
            opCtx, validateState.isFullValidation(), results);

        // For full index validation, we validate the internal structure of each index and save
        // the number of keys in the index to compare against _validateIndexes()'s count results.
        _validateIndexesInternalStructure(
            opCtx, validateState.isFullIndexValidation(), &validateState, results);

        if (!results->valid) {
            _reportInvalidResults(opCtx, &validateState, results, output);
            return Status::OK();
        }

        _validateCatalogEntry(opCtx, &validateState, results);

        if (validateState.isMetadataValidation()) {
            if (results->valid) {
                LOGV2(5980500,
                      "Validation of metadata complete for collection. No problems detected",
                      logAttrs(validateState.nss()),
                      logAttrs(validateState.uuid()));
            } else {
                LOGV2(5980501,
                      "Validation of metadata complete for collection. Problems detected",
                      logAttrs(validateState.nss()),
                      logAttrs(validateState.uuid()));
            }
            return Status::OK();
        }

        validateState.initializeCursors(opCtx);

        // Validate the record store.
        LOGV2_OPTIONS(20303,
                      {LogComponent::kIndex},
                      "validating collection",
                      logAttrs(validateState.nss()),
                      logAttrs(validateState.uuid()));

        ValidateAdaptor indexValidator(opCtx, &validateState);

        // In traverseRecordStore(), the index validator keeps track the records in the record
        // store so that _validateIndexes() can confirm that the index entries match the records in
        // the collection. For clustered collections, the validator also verifies that the
        // record key (RecordId) matches the cluster key field in the record value (document's
        // cluster key).
        indexValidator.traverseRecordStore(
            opCtx, results, output, additionalOptions.validationVersion);

        // Pause collection validation while a lock is held and between collection and index data
        // validation.
        //
        // The KeyStringIndexConsistency object saves document key information during collection
        // data validation and then compares against that key information during index data
        // validation. This fail point is placed in between them, in an attempt to catch any
        // inconsistencies that concurrent CRUD ops might cause if we were to have a bug.
        //
        // Only useful for background validation because we hold an intent lock instead of an
        // exclusive lock, and thus allow concurrent operations.

        if (MONGO_unlikely(pauseCollectionValidationWithLock.shouldFail())) {
            _validationIsPausedForTest.store(true);
            LOGV2(20304, "Failpoint 'pauseCollectionValidationWithLock' activated");
            pauseCollectionValidationWithLock.pauseWhileSet();
            _validationIsPausedForTest.store(false);
        }

        if (!results->valid) {
            _reportInvalidResults(opCtx, &validateState, results, output);
            return Status::OK();
        }

        // Validate indexes and check for mismatches.
        _validateIndexes(opCtx, &validateState, &indexValidator, results);

        if (indexValidator.haveEntryMismatch()) {
            LOGV2_OPTIONS(20305,
                          {LogComponent::kIndex},
                          "Index inconsistencies were detected. "
                          "Starting the second phase of index validation to gather concise errors",
                          logAttrs(validateState.nss()));
            _gatherIndexEntryErrors(opCtx, &validateState, &indexValidator, results);
        }

        if (!results->valid) {
            _reportInvalidResults(opCtx, &validateState, results, output);
            return Status::OK();
        }

        // Validate index key count.
        _validateIndexKeyCount(opCtx, &validateState, &indexValidator, &results->indexResultsMap);

        if (!results->valid) {
            _reportInvalidResults(opCtx, &validateState, results, output);
            return Status::OK();
        }

        // At this point, validation is complete and successful.
        // Report the validation results for the user to see.
        _reportValidationResults(opCtx, &validateState, results, output);

        LOGV2_OPTIONS(20306,
                      {LogComponent::kIndex},
                      "Validation complete for collection. No "
                      "corruption found",
                      logAttrs(validateState.nss()),
                      logAttrs(validateState.uuid()));
    } catch (const DBException& e) {
        if (!opCtx->checkForInterruptNoAssert().isOK() || e.code() == ErrorCodes::Interrupted) {
            LOGV2_OPTIONS(5160301,
                          {LogComponent::kIndex},
                          "Validation interrupted",
                          logAttrs(validateState.nss()));
            return e.toStatus();
        }

        string err = str::stream() << "exception during collection validation: " << e.toString();
        results->errors.push_back(err);
        results->valid = false;
        LOGV2_OPTIONS(5160302,
                      {LogComponent::kIndex},
                      "Validation failed due to exception",
                      logAttrs(validateState.nss()),
                      "error"_attr = e.toString());
    }

    return Status::OK();
}

bool getIsValidationPausedForTest() {
    return _validationIsPausedForTest.load();
}

}  // namespace CollectionValidation
}  // namespace mongo

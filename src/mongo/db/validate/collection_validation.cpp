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

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/viewless_timeseries_collection_creation_helpers.h"
#include "mongo/db/validate/validate_adaptor.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

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
                                       ValidateState* validateState,
                                       ValidateResults* results) {
    // Need to use the IndexCatalog here because the 'validateState->indexes' object hasn't been
    // constructed yet. It must be initialized to ensure we're validating all indexes.
    const IndexCatalog* indexCatalog = validateState->getCollection()->getIndexCatalog();
    const auto it = indexCatalog->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);

    // Validate Indexes Internal Structure, checking if index files have been compromised or
    // corrupted.
    while (it->more()) {
        opCtx->checkForInterrupt();

        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* descriptor = entry->descriptor();
        const IndexAccessMethod* iam = entry->accessMethod();

        LOGV2_PROD_ONLY_OPTIONS(20295,
                                {LogComponent::kIndex},
                                "Validating internal structure",
                                "index"_attr = descriptor->indexName(),
                                logAttrs(validateState->nss()));

        auto indexResults =
            iam->validate(opCtx, *shard_role_details::getRecoveryUnit(opCtx), *validateState);

        results->getIndexValidateResult(descriptor->indexName()) = std::move(indexResults);
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
                                          ->descriptor()
                                          ->indexName();

        const IndexType indexType = validateState->getCollection()
                                        ->getIndexCatalog()
                                        ->findIndexByIdent(opCtx, indexIdent)
                                        ->descriptor()
                                        ->getIndexType();

        LOGV2_PROD_ONLY_OPTIONS(20296,
                                {LogComponent::kIndex},
                                "Validating index consistency",
                                "indexName"_attr = indexName,
                                "indexType"_attr = indexType,
                                logAttrs(validateState->nss()));

        int64_t numTraversedKeys;
        indexValidator->traverseIndex(
            opCtx,
            validateState->getCollection()->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent),
            &numTraversedKeys,
            results);

        auto& curIndexResults = results->getIndexValidateResult(indexName);
        curIndexResults.addKeysTraversed(numTraversedKeys);

        BSONObj infoObj = validateState->getCollection()
                              ->getIndexCatalog()
                              ->findIndexByIdent(opCtx, indexIdent)
                              ->descriptor()
                              ->infoObj();
        curIndexResults.setSpec(std::move(infoObj));
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
        // Second phase doesn't report errors, but needs to know which indexes are structurally
        // sound enough to validate
        tempValidateResults.getIndexResultsMap() = result->getIndexResultsMap();
        indexValidator->traverseRecordStore(
            opCtx, &tempValidateResults, validateState->validationVersion());
    }

    LOGV2_OPTIONS(
        20298, {LogComponent::kIndex}, "Finished traversing through all the document key sets");
    LOGV2_OPTIONS(20299, {LogComponent::kIndex}, "Starting to traverse through all the indexes");

    // Iterate through all the indexes in the collection and only record the index entry keys that
    // had inconsistencies during the first phase.
    for (const auto& indexIdent : validateState->getIndexIdents()) {
        opCtx->checkForInterrupt();

        const auto indexEntry =
            validateState->getCollection()->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);

        const auto& indexValidateResult =
            result->getIndexValidateResult(indexEntry->descriptor()->indexName());
        if (!indexValidateResult.continueValidation()) {
            LOGV2(7697700,
                  "Skipping validation of index due to existing errors",
                  "indexName"_attr = indexEntry->descriptor()->indexName());
            continue;
        }

        LOGV2_OPTIONS(20300,
                      {LogComponent::kIndex},
                      "Traversing through the index entries",
                      "index"_attr = indexEntry->descriptor()->indexName());

        indexValidator->traverseIndex(opCtx,
                                      indexEntry,
                                      /*numTraversedKeys=*/nullptr,
                                      result);
    }

    if (result->getNumRemovedExtraIndexEntries() > 0) {
        result->addWarning(str::stream() << "Removed " << result->getNumRemovedExtraIndexEntries()
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
        const auto indexEntry =
            validateState->getCollection()->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
        auto& curIndexResults = (*indexNsResultsMap)[indexEntry->descriptor()->indexName()];

        if (curIndexResults.continueValidation()) {
            indexValidator->validateIndexKeyCount(opCtx, indexEntry, curIndexResults);
        }
    }
}

void _logInvalidIndices(OperationContext* opCtx,
                        ValidateState* validateState,
                        ValidateResults* results) {
    if (validateState->isFullIndexValidation()) {
        invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
            validateState->nss(), MODE_X));
    }
    for (auto& [indexName, ivr] : results->getIndexResultsMap()) {
        if (!ivr.isValid()) {
            const auto entry = validateState->getCollection()->getIndexCatalog()->findIndexByName(
                opCtx, indexName);
            auto indexSpec = entry->descriptor()->infoObj();
            LOGV2_ERROR(7463100, "Index failed validation", "spec"_attr = indexSpec);
        }
    }
}

/**
 * Logs oplog entries related to corrupted records/indexes in validation results.
 */
void _logOplogEntriesForInvalidResults(OperationContext* opCtx, ValidateResults* results) {
    if (results->getRecordTimestamps().empty()) {
        return;
    }

    LOGV2(
        7464200,
        "Validation failed: oplog timestamps referenced by corrupted collection and index entries",
        "numTimestamps"_attr = results->getRecordTimestamps().size());

    // Set up read on oplog collection.
    try {
        AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
        const auto& oplogCollection = oplogRead.getCollection();

        if (!oplogCollection) {
            for (auto it = results->getRecordTimestamps().rbegin();
                 it != results->getRecordTimestamps().rend();
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

        auto cursor = recordStore->getCursor(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx), /*forward=*/false);
        uassert(ErrorCodes::CursorNotFound,
                "Validation failed: Unable to get cursor to oplog collection.",
                cursor);

        for (auto it = results->getRecordTimestamps().rbegin();
             it != results->getRecordTimestamps().rend();
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

void _reportInvalidResults(OperationContext* opCtx,
                           ValidateState* validateState,
                           ValidateResults* results) {
    _logInvalidIndices(opCtx, validateState, results);
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
        results->addError(str::stream()
                          << "stored value for " << name
                          << " does not match cached value: " << stored << " != " << cached);
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
        results->addError("UUID missing on collection.");
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
    if (collection->isNewTimeseriesWithoutView()) {
        if (!options.validator.isEmpty()) {
            results->addError(str::stream()
                              << "Viewless time-series collection had a schema validator set in "
                                 "its metadata for collection "
                              << validateState->nss().toStringForErrorMsg());
        }
        auto validator = timeseries::generateTimeseriesValidator(
            timeseries::kTimeseriesControlLatestVersion,
            collection->getTimeseriesOptions()->getTimeField());
        addErrorIfUnequal(validator.toString(), validatorDoc.toString(), "validator", results);
    } else {
        addErrorIfUnequal(
            options.validator.toString(), validatorDoc.toString(), "validator", results);
    }
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
        results->addError(str::stream()
                          << "collection options are not valid for storage: " << options.toBSON());
    }

    const auto& indexCatalog = collection->getIndexCatalog();
    auto indexIt = indexCatalog->getIndexIterator(IndexCatalog::InclusionPolicy::kAll);

    while (indexIt->more()) {
        const IndexCatalogEntry* indexEntry = indexIt->next();
        const std::string indexName = indexEntry->descriptor()->indexName();

        Status status =
            index_key_validate::validateIndexSpec(opCtx, indexEntry->descriptor()->infoObj())
                .getStatus();
        if (!status.isOK()) {
            results->addWarning(
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
            results->addError(
                fmt::format("The 'multikey' field for index {} was false with non-empty "
                            "'multikeyPaths': {}",
                            indexName,
                            multikey_paths::toString(multikeyPaths)));
        }
    }
}

boost::optional<std::string> getConfigOverrideOrThrow(const BSONElement& raw) {
    if (!raw) {
        return boost::none;
    }
    StringData chosenConfig = raw.valueStringDataSafe();
    // Only a specific subset of valid configurations are allowlisted here. This is mostly to avoid
    // having complex logic to parse/sanitize the user-chosen configuration string.
    static const char* allowed[] = {
        "",
        "dump_address",
        "dump_blocks",
        "dump_layout",
        "dump_pages",
        "dump_tree_shape",
    };
    static const char** allowedEnd = allowed + std::size(allowed);
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Unrecognized configuration string " << chosenConfig,
            std::find(allowed, allowedEnd, chosenConfig) != allowedEnd);
    return {raw.str()};
}

}  // namespace

void validateHashes(const std::vector<std::string>& hashPrefixes, bool equalLength) {
    if (hashPrefixes.empty()) {
        return;
    }

    const size_t kHashStringMaxLen = SHA256Block().toHexString().size();
    auto hashPrefixLength = hashPrefixes[0].length();
    for (const auto& hashPrefix : hashPrefixes) {
        uassert(ErrorCodes::InvalidOptions,
                "Hash prefixes should not be empty strings.",
                !hashPrefix.empty());

        uassert(ErrorCodes::InvalidOptions,
                fmt::format("Hash prefixes too long. Received: {}", hashPrefix),
                hashPrefix.length() <= kHashStringMaxLen);

        uassert(ErrorCodes::InvalidOptions,
                "Hash prefixes should not have different lengths.",
                !equalLength || hashPrefix.length() == hashPrefixLength);

        for (char c : hashPrefix) {
            uassert(ErrorCodes::InvalidOptions,
                    fmt::format(
                        "Hash prefixes should only contain upper case hex strings. Received: {}.",
                        hashPrefix),
                    ctype::isXdigit(c) && (ctype::isUpper(c) || ctype::isDigit(c)));
        }
    }

    std::vector<std::string> sortedHashPrefixes = hashPrefixes;
    std::sort(sortedHashPrefixes.begin(), sortedHashPrefixes.end());
    for (size_t i = 0; i < sortedHashPrefixes.size() - 1; ++i) {
        const auto& currentHashPrefix = sortedHashPrefixes[i];
        const auto& nextHashPrefix = sortedHashPrefixes[i + 1];

        if (currentHashPrefix.length() <= nextHashPrefix.length()) {
            uassert(ErrorCodes::InvalidOptions,
                    fmt::format("Provided hash prefixes should not duplicate: {}, {}",
                                currentHashPrefix,
                                nextHashPrefix),
                    !nextHashPrefix.starts_with(currentHashPrefix));
        }
    }
}

ValidationOptions parseValidateOptions(OperationContext* opCtx,
                                       NamespaceString nss,
                                       const BSONObj& cmdObj) {
    const bool background = cmdObj["background"].trueValue();
    const bool logDiagnostics = cmdObj["logDiagnostics"].trueValue();

    const bool fullValidate = cmdObj["full"].trueValue();
    if (background && fullValidate) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with both { background: true }"
                                << " and { full: true } is not supported.");
    }

    const bool enforceFastCount = cmdObj["enforceFastCount"].trueValue();
    if (background && enforceFastCount) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with both { background: true }"
                                << " and { enforceFastCount: true } is not supported.");
    }

    const bool checkBSONConformance = cmdObj["checkBSONConformance"].trueValue();

    const bool repair = cmdObj["repair"].trueValue();
    if (opCtx->readOnly() && repair) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with { repair: true } in"
                                << " read-only mode is not supported.");
    }
    if (background && repair) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with both { background: true }"
                                << " and { repair: true } is not supported.");
    }
    if (enforceFastCount && repair) {
        uasserted(
            ErrorCodes::InvalidOptions,
            str::stream() << "Running the validate command with both { enforceFastCount: true }"
                          << " and { repair: true } is not supported.");
    }
    if (checkBSONConformance && repair) {
        uasserted(
            ErrorCodes::InvalidOptions,
            str::stream() << "Running the validate command with both { checkBSONConformance: true }"
                          << " and { repair: true } is not supported.");
    }
    repl::ReplicationCoordinator* replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (repair && replCoord->getSettings().isReplSet()) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with { repair: true } can only be"
                                << " performed in standalone mode.");
    }

    const auto rawFixMultikey = cmdObj["fixMultikey"];
    const bool fixMultikey = cmdObj["fixMultikey"].trueValue();
    if (fixMultikey && replCoord->getSettings().isReplSet()) {
        uasserted(
            ErrorCodes::InvalidOptions,
            str::stream() << "Running the validate command with { fixMultikey: true } can only be"
                          << " performed in standalone mode.");
    }
    if (rawFixMultikey && !fixMultikey && repair) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with both { fixMultikey: false }"
                                << " and { repair: true } is not supported.");
    }

    const bool metadata = cmdObj["metadata"].trueValue();
    if (metadata &&
        (background || fullValidate || enforceFastCount || checkBSONConformance || repair)) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with { metadata: true } is not"
                                << " supported with any other options");
    }

    const auto rawConfigOverride = cmdObj["wiredtigerVerifyConfigurationOverride"];
    if (rawConfigOverride && !(fullValidate || enforceFastCount)) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Overriding the verify configuration is only supported with "
                                   "full validation set.");
    }

    // Background validation uses point-in-time catalog lookups. This requires an instance of
    // the collection at the checkpoint timestamp. Because timestamps aren't used in standalone
    // mode, this prevents the CollectionCatalog from being able to establish the correct
    // collection instance.
    const bool isReplSet = repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();
    if (background && !isReplSet) {
        uasserted(ErrorCodes::CommandNotSupported,
                  str::stream() << "Running the validate command with { background: true } "
                                << "is not supported in standalone mode");
    }

    // The same goes for unreplicated collections, DDL operations are untimestamped.
    if (background && !nss.isReplicated()) {
        uasserted(ErrorCodes::CommandNotSupported,
                  str::stream() << "Running the validate command with { background: true } "
                                << "is not supported on unreplicated collections");
    }

    // collHash parameter.
    const bool collHash = cmdObj["collHash"].trueValue();

    // hashPrefixes parameter.
    const auto rawHashPrefixes = cmdObj["hashPrefixes"];
    boost::optional<std::vector<std::string>> hashPrefixes = boost::none;
    if (rawHashPrefixes) {
        hashPrefixes = std::vector<std::string>();
        for (const auto& e : rawHashPrefixes.Array()) {
            hashPrefixes->push_back(boost::algorithm::to_upper_copy(e.String()));
        }
        CollectionValidation::validateHashes(*hashPrefixes, /*equalLength=*/true);
        if (!hashPrefixes->size()) {
            hashPrefixes->push_back(std::string(""));
        }
    }
    if (rawHashPrefixes && replCoord->getSettings().isReplSet()) {
        uasserted(
            ErrorCodes::InvalidOptions,
            str::stream() << "Running the validate command with { hashPrefixes: [] } can only be"
                          << " performed in standalone mode.");
    }
    if (rawHashPrefixes && !collHash) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with { hashPrefixes: [] }"
                                << " requires {collHash: true}.");
    }

    // revealHashedIds parameter.
    const auto rawRevealHashedIds = cmdObj["revealHashedIds"];
    boost::optional<std::vector<std::string>> revealHashedIds = boost::none;
    if (rawRevealHashedIds && replCoord->getSettings().isReplSet()) {
        uasserted(
            ErrorCodes::InvalidOptions,
            str::stream() << "Running the validate command with { revealHashedIds: [] } can only be"
                          << " performed in standalone mode.");
    }
    if (rawRevealHashedIds && !collHash) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with { revealHashedIds: [] }"
                                << " requires {collHash: true}.");
    }
    if (rawRevealHashedIds && rawHashPrefixes) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Running the validate command with { revealHashedIds: [] } "
                                   "cannot be done with"
                                << " {hashPrefixes: []}.");
    }
    if (rawRevealHashedIds) {
        const auto& rawRevealHashedIdsArr = rawRevealHashedIds.Array();
        if (rawRevealHashedIdsArr.empty()) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with { revealHashedIds: [] } "
                                       "cannot be done with"
                                    << " an empty array provided.");
        }
        revealHashedIds = std::vector<std::string>();
        for (const auto& e : rawRevealHashedIdsArr) {
            revealHashedIds->push_back(boost::algorithm::to_upper_copy(e.String()));
        }
        CollectionValidation::validateHashes(*revealHashedIds, /*equalLength=*/false);
    }

    boost::optional<Timestamp> timestamp = boost::none;
    if (cmdObj["atClusterTime"]) {
        if (background) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with { background: true } "
                                       "cannot be done with {atClusterTime: ...} because "
                                       "background already sets a read timestamp.");
        }
        if (!nss.isReplicated()) {
            uasserted(ErrorCodes::CommandNotSupported,
                      str::stream() << "Running the validate command with { atClusterTime: ... } "
                                    << "is not supported on unreplicated collections");
        }
        timestamp = cmdObj["atClusterTime"].timestamp();
    }
    if (background) {
        // Background validation reads data from the last stable checkpoint.
        timestamp =
            opCtx->getServiceContext()->getStorageEngine()->getLastStableRecoveryTimestamp();
        if (!timestamp) {
            uasserted(ErrorCodes::NamespaceNotFound,
                      fmt::format("Cannot run background validation on collection because there "
                                  "is no checkpoint yet"));
        }
    }

    const auto validateMode = [&] {
        if (metadata) {
            return CollectionValidation::ValidateMode::kMetadata;
        }
        if (hashPrefixes) {
            return CollectionValidation::ValidateMode::kHashDrillDown;
        }
        if (collHash) {
            return CollectionValidation::ValidateMode::kCollectionHash;
        }
        if (background) {
            return checkBSONConformance ? CollectionValidation::ValidateMode::kBackgroundCheckBSON
                                        : CollectionValidation::ValidateMode::kBackground;
        }
        if (enforceFastCount) {
            return CollectionValidation::ValidateMode::kForegroundFullEnforceFastCount;
        }
        if (fullValidate) {
            return checkBSONConformance
                ? CollectionValidation::ValidateMode::kForegroundFullCheckBSON
                : CollectionValidation::ValidateMode::kForegroundFull;
        }
        if (checkBSONConformance) {
            return CollectionValidation::ValidateMode::kForegroundCheckBSON;
        }
        return CollectionValidation::ValidateMode::kForeground;
    }();

    const auto repairMode = [&] {
        if (opCtx->readOnly()) {
            // On read-only mode we can't make any adjustments.
            return CollectionValidation::RepairMode::kNone;
        }
        switch (validateMode) {
            case CollectionValidation::ValidateMode::kForeground:
            case CollectionValidation::ValidateMode::kCollectionHash:
            case CollectionValidation::ValidateMode::kHashDrillDown:
            case CollectionValidation::ValidateMode::kForegroundCheckBSON:
            case CollectionValidation::ValidateMode::kForegroundFull:
            case CollectionValidation::ValidateMode::kForegroundFullIndexOnly:
                // Foreground validation may not repair data while running as a replica set node
                // because we do not have timestamps that are required to perform writes.
                if (replCoord->getSettings().isReplSet()) {
                    return CollectionValidation::RepairMode::kNone;
                }
                if (repair) {
                    return CollectionValidation::RepairMode::kFixErrors;
                }
                if (fixMultikey) {
                    return CollectionValidation::RepairMode::kAdjustMultikey;
                }
                return CollectionValidation::RepairMode::kNone;
            default:
                return CollectionValidation::RepairMode::kNone;
        }
    }();

    if (repair) {
        shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
    }

    return {validateMode,
            repairMode,
            logDiagnostics,
            getTestCommandsEnabled() ? (ValidationVersion)bsonTestValidationVersion
                                     : currentValidationVersion,
            getConfigOverrideOrThrow(rawConfigOverride),
            timestamp,
            hashPrefixes,
            revealHashedIds};
}

Status validate(OperationContext* opCtx,
                const NamespaceString& nss,
                ValidationOptions options,
                ValidateResults* results) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked() || storageGlobalParams.repair ||
              storageGlobalParams.validate);

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

    // This is deliberately outside of the try-catch block, so that any errors thrown in the
    // constructor fail the cmd, as opposed to returning OK with valid:false.
    ValidateState validateState(opCtx, nss, std::move(options));

    // Relax corruption detection so that we log and continue scanning instead of failing early.
    auto oldDataCorruptionMode =
        shard_role_details::getRecoveryUnit(opCtx)->getDataCorruptionDetectionMode();
    shard_role_details::getRecoveryUnit(opCtx)->setDataCorruptionDetectionMode(
        DataCorruptionDetectionMode::kLogAndContinue);
    ON_BLOCK_EXIT([&] {
        shard_role_details::getRecoveryUnit(opCtx)->setDataCorruptionDetectionMode(
            oldDataCorruptionMode);
    });

    results->setRepairMode(validateState.getRepairMode());

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

    if (gFeatureFlagPrefetch.isEnabled() &&
        !opCtx->getServiceContext()->getStorageEngine()->isEphemeral()) {
        shard_role_details::getRecoveryUnit(opCtx)->setPrefetching(true);
    }

    Status status = validateState.initializeCollection(opCtx);
    uassertStatusOK(status);

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    // Check whether we are allowed to read from this node after acquiring our locks. If we are
    // in a state where we cannot read, we should not run validate.
    uassertStatusOK(replCoord->checkCanServeReadsFor(
        opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

    results->setNamespaceString(validateState.nss());
    results->setUUID(validateState.uuid());
    results->setReadTimestamp(validateState.getReadTimestamp());

    try {
        invariant(!validateState.isFullIndexValidation() ||
                  shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                      validateState.nss(), MODE_X));

        if (validateState.isHashDrillDown()) {
            validateState.initializeCursors(opCtx);
            ValidateAdaptor recordStoreValidator(opCtx, &validateState);
            recordStoreValidator.hashDrillDown(opCtx, results);
            return Status::OK();
        }

        // Record store validation code is executed before we open cursors because it may close
        // and/or invalidate all open cursors.
        validateState.getCollection()->getRecordStore()->validate(
            *shard_role_details::getRecoveryUnit(opCtx), validateState, results);

        // For full index validation, we validate the internal structure of each index and save
        // the number of keys in the index to compare against _validateIndexes()'s count results.
        _validateIndexesInternalStructure(opCtx, &validateState, results);

        if (!results->isValid()) {
            _reportInvalidResults(opCtx, &validateState, results);
            return Status::OK();
        }

        _validateCatalogEntry(opCtx, &validateState, results);

        if (validateState.isMetadataValidation()) {
            if (results->isValid()) {
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
        indexValidator.traverseRecordStore(opCtx, results, validateState.validationVersion());

        if (validateState.isCollHashValidation()) {
            indexValidator.computeMetadataHash(opCtx, validateState.getCollection(), results);
        }

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

        // Continue validation checks are done in case previously reported errors need additional
        // metadata to be added by later calls
        if (!results->isValid() && !results->continueValidation()) {
            _reportInvalidResults(opCtx, &validateState, results);
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

        if (!results->isValid() && !results->continueValidation()) {
            _reportInvalidResults(opCtx, &validateState, results);
            return Status::OK();
        }

        // Validate index key count.
        _validateIndexKeyCount(
            opCtx, &validateState, &indexValidator, &results->getIndexResultsMap());

        // We don't want to check continueValidation as there are no more validation checks to do
        if (!results->isValid()) {
            _reportInvalidResults(opCtx, &validateState, results);
            return Status::OK();
        }

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
        results->addWarning(err);
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

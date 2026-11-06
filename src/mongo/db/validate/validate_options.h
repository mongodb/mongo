// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo::collection_validation {

enum class ValidateMode {
    // Only performs validation on the collection metadata.
    kMetadata,
    // Does the above, plus checks a collection's data and indexes for correctness in a non-blocking
    // manner using an intent collection lock.
    kBackground,
    // Does the above, plus checks BSON documents more thoroughly.
    kBackgroundCheckBSON,
    // Does the above, but in a blocking manner using an exclusive collection lock.
    kForeground,

    // The standard foreground validation above, plus a full validation of the underlying
    // SortedDataInterface using the storage engine's validation functionality. For WiredTiger,
    // this results in a call to verify() for each index.
    //
    // This mode is only used by repair to avoid revalidating the record store.
    kForegroundFullIndexOnly,

    // The standard foreground validation above, plus a more thorough BSON document validation.
    kForegroundCheckBSON,

    // The full index validation plus a full validation of the underlying record store using the
    // storage engine's validation functionality. For WiredTiger, this results in a call to
    // verify().
    kForegroundFull,

    // The full validation above including thorough BSON document validation above, plus a full
    // validation of the underlying record store using the storage engine's validation
    // functionality. For WiredTiger, this results in a call to verify().
    kForegroundFullCheckBSON,

    // The full index and record store validation above, plus enforce that the fast count is equal
    // to the number of records (as opposed to correcting the fast count if it is incorrect).
    kForegroundFullEnforceFastCount,

    // The full index and record store validation above, plus enforce that the fast size is equal
    // to the size of the records (as opposed to correcting the fast size if it is incorrect).
    kForegroundFullEnforceFastSize,

    // The full index and record store validation above, plus enforce that the fast count is equal
    // to the number of records and the fast size is equal to the size of these records. (as opposed
    // to correcting the fast count/size if they are incorrect).
    kForegroundFullEnforceFastCountAndSize,

    // Performs an extended validate where a total collection hash is computed and returned,
    // alongside the behavior performed in kForegroundFull.
    kCollectionHash,

    // Part of extended validate, perform drill down operations to isolate documents where
    // inconsistencies are found. Performs no other validation checks.
    kHashDrillDown,
};

/**
 * RepairMode indicates whether validate should repair the data inconsistencies it detects.
 *
 * When set to kFixErrors, if any validation errors are detected, repairs are attempted and the
 * 'repaired' flag in ValidateResults will be set to true. If all errors are fixed, then 'valid'
 * will also be set to true. kFixErrors is incompatible with the ValidateModes kBackground and
 * kForegroundFullEnforceFast[Count|Size|CountAndSize]. This implies kAdjustMultikey.
 *
 * When set to kAdjustMultikey, if any permissible, yet undesirable multikey inconsistencies are
 * detected, then the multikey metadata will be adjusted. The 'repaired' flag will be set if any
 * adjustments have been made. This is incompatible with background validation.
 */
enum class RepairMode {
    kNone,
    kFixErrors,
    kAdjustMultikey,
};

std::string_view toString(ValidateMode validateMode);
std::string_view toString(RepairMode repairMode);

/**
 * Additional validation options that can run in any mode.
 */
struct ValidationOptions {

    bool isMetadataValidation() const {
        return validateMode == ValidateMode::kMetadata;
    }

    bool isBackground() const {
        return validateMode == ValidateMode::kBackground ||
            validateMode == ValidateMode::kBackgroundCheckBSON;
    }

    bool isFullValidation() const {
        return validateMode == ValidateMode::kForegroundFull ||
            validateMode == ValidateMode::kForegroundFullCheckBSON ||
            validateMode == ValidateMode::kForegroundFullEnforceFastCount ||
            validateMode == ValidateMode::kForegroundFullEnforceFastSize ||
            validateMode == ValidateMode::kForegroundFullEnforceFastCountAndSize;
    }

    bool isFullIndexValidation() const {
        return isFullValidation() || validateMode == ValidateMode::kForegroundFullIndexOnly;
    }

    bool isBSONConformanceValidation() const {
        return validateMode == ValidateMode::kForegroundFullCheckBSON ||
            validateMode == ValidateMode::kBackgroundCheckBSON ||
            validateMode == ValidateMode::kForegroundCheckBSON;
    }

    bool isExtendedValidation() const {
        return validateMode == ValidateMode::kCollectionHash ||
            validateMode == ValidateMode::kHashDrillDown;
    }

    bool isCollHashValidation() const {
        return validateMode == ValidateMode::kCollectionHash;
    }

    bool isHashDrillDown() const {
        return validateMode == ValidateMode::kHashDrillDown;
    }

    /**
     * Returns true iff the validation was *asked* to enforce the fast count, whether it actually
     * does depends on what collection is being validated and what the other options are. See
     * ValidateState::shouldEnforceFastCount().
     */
    bool enforceFastCountRequested() const {
        return validateMode == ValidateMode::kForegroundFullEnforceFastCount ||
            validateMode == ValidateMode::kForegroundFullEnforceFastCountAndSize;
    }

    /**
     * Returns true iff the validation was *asked* to enforce the fast size, whether it actually
     * does depends on what collection is being validated and what the other options are. See
     * ValidateState::shouldEnforceFastCount().
     */
    bool enforceFastSizeRequested() const {
        return validateMode == ValidateMode::kForegroundFullEnforceFastSize ||
            validateMode == ValidateMode::kForegroundFullEnforceFastCountAndSize;
    }

    bool fixErrors() const {
        return repairMode == RepairMode::kFixErrors;
    }

    bool adjustMultikey() const {
        return repairMode == RepairMode::kFixErrors || repairMode == RepairMode::kAdjustMultikey;
    }

    /**
     * The options this validation resolved to, for reporting. The mode alone determines most of
     * what a validation does, and nothing else logs it, so a run's workload cannot otherwise be
     * identified from its output.
     */
    BSONObj toBSON() const;

    ValidateMode validateMode = ValidateMode::kForeground;

    RepairMode repairMode = RepairMode::kNone;

    // Can be set to obtain better insight into what validate sees/does.
    bool logDiagnostics = false;

    ValidationVersion validationVersion = currentValidationVersion;

    boost::optional<std::string> verifyConfigurationOverride;

    boost::optional<Timestamp> readTimestamp;

    boost::optional<std::vector<std::string>> hashPrefixes;

    boost::optional<std::vector<std::string>> revealHashedIds;

    boost::optional<int64_t> targetRecordsPerRecordStoreSlice;

    // Opt-in: when true (and running collHash validation), the record store and index scans
    // accumulate a storage size summary that is read back and logged.
    bool sizeStats = false;
};

}  // namespace mongo::collection_validation

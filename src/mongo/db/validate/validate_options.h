// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bson_validate.h"
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

/**
 * Additional validation options that can run in any mode.
 */
class ValidationOptions {
public:
    ValidationOptions(ValidateMode validateMode,
                      RepairMode repairMode,
                      bool logDiagnostics,
                      ValidationVersion validationVersion = currentValidationVersion,
                      boost::optional<std::string> verifyConfigurationOverride = boost::none,
                      boost::optional<Timestamp> readTimestamp = boost::none,
                      boost::optional<std::vector<std::string>> hashPrefixes = boost::none,
                      boost::optional<std::vector<std::string>> revealHashedIds = boost::none);

    virtual ~ValidationOptions() = default;

    bool isMetadataValidation() const {
        return _validateMode == ValidateMode::kMetadata;
    }

    bool isBackground() const {
        return _validateMode == ValidateMode::kBackground ||
            _validateMode == ValidateMode::kBackgroundCheckBSON;
    }

    bool isFullValidation() const {
        return _validateMode == ValidateMode::kForegroundFull ||
            _validateMode == ValidateMode::kForegroundFullCheckBSON ||
            _validateMode == ValidateMode::kForegroundFullEnforceFastCount ||
            _validateMode == ValidateMode::kForegroundFullEnforceFastSize ||
            _validateMode == ValidateMode::kForegroundFullEnforceFastCountAndSize;
    }

    bool isFullIndexValidation() const {
        return isFullValidation() || _validateMode == ValidateMode::kForegroundFullIndexOnly;
    }

    bool isBSONConformanceValidation() const {
        return _validateMode == ValidateMode::kForegroundFullCheckBSON ||
            _validateMode == ValidateMode::kBackgroundCheckBSON ||
            _validateMode == ValidateMode::kForegroundCheckBSON;
    }

    bool isExtendedValidation() const {
        return _validateMode == ValidateMode::kCollectionHash ||
            _validateMode == ValidateMode::kHashDrillDown;
    }

    bool isCollHashValidation() const {
        return _validateMode == ValidateMode::kCollectionHash;
    }

    bool isHashDrillDown() const {
        return _validateMode == ValidateMode::kHashDrillDown;
    }

    /**
     * Returns true iff the validation was *asked* to enforce the fast count, whether it actually
     * does depends on what collection is being validated and what the other options are. See
     * ValidateState::shouldEnforceFastCount().
     */
    bool enforceFastCountRequested() const {
        return _validateMode == ValidateMode::kForegroundFullEnforceFastCount ||
            _validateMode == ValidateMode::kForegroundFullEnforceFastCountAndSize;
    }

    /**
     * Returns true iff the validation was *asked* to enforce the fast size, whether it actually
     * does depends on what collection is being validated and what the other options are. See
     * ValidateState::shouldEnforceFastCount().
     */
    bool enforceFastSizeRequested() const {
        return _validateMode == ValidateMode::kForegroundFullEnforceFastSize ||
            _validateMode == ValidateMode::kForegroundFullEnforceFastCountAndSize;
    }

    const boost::optional<Timestamp>& getReadTimestamp() const {
        return _readTimestamp;
    }

    const boost::optional<std::vector<std::string>>& getHashPrefixes() const {
        return _hashPrefixes;
    }

    const boost::optional<std::vector<std::string>>& getRevealHashedIds() const {
        return _revealHashedIds;
    }

    RepairMode getRepairMode() const {
        return _repairMode;
    }

    bool fixErrors() const {
        return _repairMode == RepairMode::kFixErrors;
    }

    bool adjustMultikey() const {
        return _repairMode == RepairMode::kFixErrors || _repairMode == RepairMode::kAdjustMultikey;
    }

    /**
     * Indicates whether extra logging should occur during validation.
     */
    bool logDiagnostics() const {
        return _logDiagnostics;
    }

    ValidationVersion validationVersion() const {
        return _validationVersion;
    }

    const boost::optional<std::string>& verifyConfigurationOverride() const {
        return _verifyConfigurationOverride;
    }

private:
    ValidateMode _validateMode;

    RepairMode _repairMode;

    // Can be set to obtain better insight into what validate sees/does.
    bool _logDiagnostics;

    ValidationVersion _validationVersion;

    boost::optional<std::string> _verifyConfigurationOverride;

    boost::optional<Timestamp> _readTimestamp;

    boost::optional<std::vector<std::string>> _hashPrefixes;

    boost::optional<std::vector<std::string>> _revealHashedIds;
};

}  // namespace mongo::collection_validation

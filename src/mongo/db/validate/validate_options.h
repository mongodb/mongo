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

#pragma once

#include "mongo/bson/bson_validate.h"

namespace mongo::CollectionValidation {

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

    // The full index validation and more thorough BSON document validation above, plus a full
    // validation of the underlying record store using the storage engine's validation
    // functionality. For WiredTiger, this results in a call to
    // verify().
    kForegroundFull,

    // The full index, BSON document, and record store validation above, plus enforce that the fast
    // count is equal to the number of records (as opposed to correcting the fast count if it is
    // incorrect).
    kForegroundFullEnforceFastCount,

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
 * kForegroundFullEnforceFastCount. This implies kAdjustMultikey.
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
                      boost::optional<std::vector<BSONElement>> hashPrefixes = boost::none,
                      boost::optional<std::vector<BSONElement>> unhash = boost::none);

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
            _validateMode == ValidateMode::kForegroundFullEnforceFastCount ||
            _validateMode == ValidateMode::kCollectionHash;
    }

    bool isFullIndexValidation() const {
        return isFullValidation() || _validateMode == ValidateMode::kForegroundFullIndexOnly;
    }

    bool isBSONConformanceValidation() const {
        return isFullValidation() || _validateMode == ValidateMode::kBackgroundCheckBSON ||
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
        return _validateMode == ValidateMode::kForegroundFullEnforceFastCount;
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
    bool logDiagnostics() {
        return _logDiagnostics;
    }

    ValidationVersion validationVersion() const {
        return _validationVersion;
    }

    const boost::optional<std::string>& verifyConfigurationOverride() const {
        return _verifyConfigurationOverride;
    }

private:
    const ValidateMode _validateMode;

    const RepairMode _repairMode;

    // Can be set to obtain better insight into what validate sees/does.
    const bool _logDiagnostics;

    const ValidationVersion _validationVersion;

    const boost::optional<std::string> _verifyConfigurationOverride;

    const boost::optional<std::vector<BSONElement>> _hashPrefixes;
    const boost::optional<std::vector<BSONElement>> _unhash;
};

}  // namespace mongo::CollectionValidation

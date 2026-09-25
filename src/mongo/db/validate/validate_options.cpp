// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_options.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

namespace mongo::collection_validation {

std::string_view toString(ValidateMode validateMode) {
    switch (validateMode) {
        case ValidateMode::kMetadata:
            return "metadata";
        case ValidateMode::kBackground:
            return "background";
        case ValidateMode::kBackgroundCheckBSON:
            return "backgroundCheckBSON";
        case ValidateMode::kForeground:
            return "foreground";
        case ValidateMode::kForegroundFullIndexOnly:
            return "foregroundFullIndexOnly";
        case ValidateMode::kForegroundCheckBSON:
            return "foregroundCheckBSON";
        case ValidateMode::kForegroundFull:
            return "foregroundFull";
        case ValidateMode::kForegroundFullCheckBSON:
            return "foregroundFullCheckBSON";
        case ValidateMode::kForegroundFullEnforceFastCount:
            return "foregroundFullEnforceFastCount";
        case ValidateMode::kForegroundFullEnforceFastSize:
            return "foregroundFullEnforceFastSize";
        case ValidateMode::kForegroundFullEnforceFastCountAndSize:
            return "foregroundFullEnforceFastCountAndSize";
        case ValidateMode::kCollectionHash:
            return "collectionHash";
        case ValidateMode::kHashDrillDown:
            return "hashDrillDown";
    }
    MONGO_UNREACHABLE;
}

std::string_view toString(RepairMode repairMode) {
    switch (repairMode) {
        case RepairMode::kNone:
            return "none";
        case RepairMode::kFixErrors:
            return "fixErrors";
        case RepairMode::kAdjustMultikey:
            return "adjustMultikey";
    }
    MONGO_UNREACHABLE;
}

BSONObj ValidationOptions::toBSON() const {
    BSONObjBuilder builder;
    builder.append("mode", toString(_validateMode));
    builder.append("repairMode", toString(_repairMode));
    builder.append("repair", fixErrors());
    builder.append("fixMultikey", adjustMultikey());
    builder.append("logDiagnostics", _logDiagnostics);
    builder.append("validationVersion", static_cast<int>(_validationVersion));
    builder.append("sizeStats", _sizeStats);
    if (_verifyConfigurationOverride) {
        builder.append("verifyConfigurationOverride", *_verifyConfigurationOverride);
    }
    if (_readTimestamp) {
        builder.append("readTimestamp", *_readTimestamp);
    }
    if (_hashPrefixes) {
        builder.append("hashPrefixes", *_hashPrefixes);
    }
    if (_revealHashedIds) {
        builder.append("revealHashedIds", *_revealHashedIds);
    }
    if (_targetRecordsPerRecordStoreSlice) {
        builder.append("targetRecordsPerRecordStoreSlice", *_targetRecordsPerRecordStoreSlice);
    }
    return builder.obj();
}

ValidationOptions::ValidationOptions(ValidateMode validateMode,
                                     RepairMode repairMode,
                                     bool logDiagnostics,
                                     ValidationVersion validationVersion,
                                     boost::optional<std::string> verifyConfigurationOverride,
                                     boost::optional<Timestamp> readTimestamp,
                                     boost::optional<std::vector<std::string>> hashPrefixes,
                                     boost::optional<std::vector<std::string>> revealHashedIds,
                                     boost::optional<int64_t> targetRecordsPerRecordStoreSlice,
                                     bool sizeStats)
    : _validateMode(validateMode),
      _repairMode(repairMode),
      _logDiagnostics(logDiagnostics),
      _validationVersion(validationVersion),
      _verifyConfigurationOverride(std::move(verifyConfigurationOverride)),
      _readTimestamp(readTimestamp),
      _hashPrefixes(std::move(hashPrefixes)),
      _revealHashedIds(std::move(revealHashedIds)),
      _targetRecordsPerRecordStoreSlice(targetRecordsPerRecordStoreSlice),
      _sizeStats(sizeStats) {}

}  // namespace mongo::collection_validation

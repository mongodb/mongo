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
    builder.append("mode", toString(validateMode));
    builder.append("repairMode", toString(repairMode));
    builder.append("repair", fixErrors());
    builder.append("fixMultikey", adjustMultikey());
    builder.append("logDiagnostics", logDiagnostics);
    builder.append("validationVersion", static_cast<int>(validationVersion));
    builder.append("sizeStats", sizeStats);
    if (verifyConfigurationOverride) {
        builder.append("verifyConfigurationOverride", *verifyConfigurationOverride);
    }
    if (readTimestamp) {
        builder.append("readTimestamp", *readTimestamp);
    }
    if (hashPrefixes) {
        builder.append("hashPrefixes", *hashPrefixes);
    }
    if (revealHashedIds) {
        builder.append("revealHashedIds", *revealHashedIds);
    }
    if (targetRecordsPerRecordStoreSlice) {
        builder.append("targetRecordsPerRecordStoreSlice", *targetRecordsPerRecordStoreSlice);
    }
    return builder.obj();
}

}  // namespace mongo::collection_validation

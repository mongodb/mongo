// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/rollover.h"

namespace mongo::timeseries::bucket_catalog {
RolloverAction getRolloverAction(RolloverReason reason) {
    switch (reason) {
        case RolloverReason::kCount:
        case RolloverReason::kSchemaChange:
        case RolloverReason::kCachePressure:
        case RolloverReason::kSize:
            return RolloverAction::kHardClose;
        case RolloverReason::kTimeForward:
            return RolloverAction::kSoftClose;
        case RolloverReason::kTimeBackward:
            return RolloverAction::kArchive;
        default:
            return RolloverAction::kNone;
    }
}
}  // namespace mongo::timeseries::bucket_catalog

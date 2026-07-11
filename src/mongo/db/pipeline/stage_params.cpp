// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/stage_params.h"

#include "mongo/base/init.h"

#include <string_view>

namespace mongo {

StageParams::Id StageParams::allocateId(std::string_view name) {
    static Atomic<Id> next{kUnallocatedId + 1};
    auto id = next.fetchAndAdd(1);
    return id;
}

DefaultStageParams::DefaultStageParams(BSONElement originalSpec) : _originalSpec(originalSpec) {};


BSONElement DefaultStageParams::getOriginalBson() const {
    return _originalSpec;
}

// Define initializer groups for StageParams ID allocation.
// These groups ensure that all StageParams IDs are allocated between BeginStageIdAllocation
// and EndStageIdAllocation, providing a well-defined initialization phase.
MONGO_INITIALIZER_GROUP(BeginStageIdAllocation, ("default"), ("EndStageIdAllocation"))
MONGO_INITIALIZER_GROUP(EndStageIdAllocation, ("BeginStageIdAllocation"), ())

}  // namespace mongo

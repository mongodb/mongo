// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * TimestampBlock is an raii type that sets a commit timestamp on the RecoveryUnit at construction
 * and clears it at destruction. RecoveryUnit::setTimestamp() should not be called while a
 * TimestampBlock is in scope.
 */
class TimestampBlock {
    TimestampBlock(const TimestampBlock&) = delete;
    TimestampBlock& operator=(const TimestampBlock&) = delete;

public:
    TimestampBlock(OperationContext* opCtx, Timestamp ts);
    ~TimestampBlock();

private:
    OperationContext* const _opCtx;
    Timestamp _ts;
};

}  // namespace mongo

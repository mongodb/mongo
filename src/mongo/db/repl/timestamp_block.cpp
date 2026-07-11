// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/timestamp_block.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"

namespace mongo {

TimestampBlock::TimestampBlock(OperationContext* opCtx, Timestamp ts) : _opCtx(opCtx), _ts(ts) {
    uassert(ErrorCodes::IllegalOperation,
            "Cannot timestamp a write operation in read-only mode",
            !_opCtx->readOnly());
    if (!_ts.isNull()) {
        shard_role_details::getRecoveryUnit(_opCtx)->setCommitTimestamp(_ts);
    }
}

TimestampBlock::~TimestampBlock() {
    if (!_ts.isNull()) {
        shard_role_details::getRecoveryUnit(_opCtx)->clearCommitTimestamp();
    }
}

}  // namespace mongo

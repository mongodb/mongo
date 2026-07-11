// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/scoped_read_concern.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/shard_role/transaction_resources.h"

namespace mongo {


ScopedReadConcern::ScopedReadConcern(OperationContext* opCtx,
                                     repl::ReadConcernArgs requestReadConcernArgs)
    : _opCtx(opCtx) {
    _originalRCA = repl::ReadConcernArgs::get(_opCtx);
    _originalReadSource = shard_role_details::getRecoveryUnit(_opCtx)->getTimestampReadSource();
    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided)
        _originalReadTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();

    std::lock_guard<Client> lk(*_opCtx->getClient());
    repl::ReadConcernArgs::get(opCtx) = requestReadConcernArgs;
}

ScopedReadConcern::~ScopedReadConcern() {
    std::lock_guard<Client> lk(*_opCtx->getClient());
    repl::ReadConcernArgs::get(_opCtx) = _originalRCA;
    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided) {
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(_originalReadSource,
                                                                            _originalReadTimestamp);
    } else {
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(_originalReadSource);
    }
}

}  // namespace mongo

/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/scoped_read_concern.h"

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"

namespace mongo {


ScopedReadConcern::ScopedReadConcern(OperationContext* opCtx,
                                     repl::ReadConcernArgs requestReadConcernArgs)
    : _opCtx(opCtx) {
    _originalRCA = repl::ReadConcernArgs::get(_opCtx);
    _originalReadSource = shard_role_details::getRecoveryUnit(_opCtx)->getTimestampReadSource();
    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided)
        _originalReadTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();

    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    repl::ReadConcernArgs::get(opCtx) = requestReadConcernArgs;
}

ScopedReadConcern::~ScopedReadConcern() {
    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    repl::ReadConcernArgs::get(_opCtx) = _originalRCA;
    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided) {
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(_originalReadSource,
                                                                            _originalReadTimestamp);
    } else {
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(_originalReadSource);
    }
}

}  // namespace mongo

/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/ops/write_ops_exec_util.h"

#include "mongo/db/s/collection_sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

namespace mongo::write_ops_exec {

LastOpFixer::LastOpFixer(OperationContext* opCtx) : _opCtx(opCtx) {}

LastOpFixer::~LastOpFixer() {
    // We don't need to do this if we are in a multi-document transaction as read-only/noop
    // transactions will always write another noop entry at transaction commit time which we can
    // use to wait for writeConcern.
    if (!_opCtx->inMultiDocumentTransaction() && _needToFixLastOp) {
        // If this operation has already generated a new lastOp, don't bother setting it
        // here. No-op updates will not generate a new lastOp, so we still need the
        // guard to fire in that case.
        replClientInfo().setLastOpToSystemLastOpTimeIgnoringCtxInterrupted(_opCtx);
        LOGV2_DEBUG(20888,
                    5,
                    "Set last op to system time: {timestamp}",
                    "Set last op to system time",
                    "timestamp"_attr = replClientInfo().getLastOp().getTimestamp());
    }
}

void LastOpFixer::startingOp(const NamespaceString& ns) {
    // Operations on the local DB aren't replicated, so they don't need to bump the lastOp.
    _needToFixLastOp = !ns.isLocalDB();
    _opTimeAtLastOpStart = replClientInfo().getLastOp();
}

void LastOpFixer::finishedOpSuccessfully() {
    // If we intended to fix the LastOp for this operation when it started, fix it now
    // if it was a no-op write. If the op was successful and already bumped LastOp itself,
    // we don't need to do it again.
    _needToFixLastOp = _needToFixLastOp && (replClientInfo().getLastOp() == _opTimeAtLastOpStart);
}

void assertCanWrite_inlock(OperationContext* opCtx, const NamespaceString& nss) {
    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Not primary while writing to " << nss.toStringForErrorMsg(),
            repl::ReplicationCoordinator::get(opCtx->getServiceContext())
                ->canAcceptWritesFor(opCtx, nss));

    CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
        ->checkShardVersionOrThrow(opCtx);
}

}  // namespace mongo::write_ops_exec

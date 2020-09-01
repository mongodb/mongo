/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_client_info.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

namespace mongo {
namespace repl {

const Client::Decoration<ReplClientInfo> ReplClientInfo::forClient =
    Client::declareDecoration<ReplClientInfo>();

namespace {
// We use a struct to wrap lastOpSetExplicitly here in order to give the boolean a default value
// when initially constructed for the associated OperationContext.
struct LastOpInfo {
    bool lastOpSetExplicitly = false;
};
static const OperationContext::Decoration<LastOpInfo> lastOpInfo =
    OperationContext::declareDecoration<LastOpInfo>();
}  // namespace

bool ReplClientInfo::lastOpWasSetExplicitlyByClientForCurrentOperation(
    OperationContext* opCtx) const {
    return lastOpInfo(opCtx).lastOpSetExplicitly;
}

void ReplClientInfo::setLastOp(OperationContext* opCtx, const OpTime& ot) {
    invariant(ot >= _lastOp);
    _lastOp = ot;
    lastOpInfo(opCtx).lastOpSetExplicitly = true;
}

void ReplClientInfo::setLastOpToSystemLastOpTime(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
    if (replCoord->isReplEnabled() && opCtx->writesAreReplicated()) {
        auto latestWriteOpTimeSW = replCoord->getLatestWriteOpTime(opCtx);
        auto status = latestWriteOpTimeSW.getStatus();
        OpTime systemOpTime;
        if (status.isOK()) {
            systemOpTime = latestWriteOpTimeSW.getValue();
        } else {
            // Fall back to use my lastAppliedOpTime if we failed to get the latest OpTime from
            // storage. In most cases, it is safe to ignore errors because if
            // getLatestWriteOpTime returns an error, we cannot use the same opCtx to wait for
            // writeConcern anyways. But getLastError from the same client could use a different
            // opCtx to wait for the lastOp. So this is a best effort attempt to set the lastOp
            // to the in-memory lastAppliedOpTime (which could be lagged). And this is a known
            // bug in getLastError.
            systemOpTime = replCoord->getMyLastAppliedOpTime();
            if (status == ErrorCodes::OplogOperationUnsupported ||
                status == ErrorCodes::NamespaceNotFound ||
                status == ErrorCodes::CollectionIsEmpty || ErrorCodes::isNotPrimaryError(status)) {
                // It is ok if the storage engine does not support getLatestOplogTimestamp() or
                // if the oplog is empty. If the node stepped down in between, it is correct to
                // use lastAppliedOpTime as last OpTime.
                status = Status::OK();
            }
            // We will continue trying to set client's lastOp to lastAppliedOpTime as a best-effort
            // alternative to getLatestWriteOpTime. And we will then throw after setting client's
            // lastOp if getLatestWriteOpTime has failed with a error code other than the ones
            // above.
        }

        // If the system timestamp has gone backwards, that must mean that there was a rollback.
        // If the system optime has a higher term but a lower timestamp than the client's lastOp, it
        // means that this node's wallclock time was ahead of the current primary's before it rolled
        // back. This is safe, but the timestamp of the last op for a Client should never go
        // backwards, so just leave the last op for this Client as it was.
        if (systemOpTime.getTerm() >= _lastOp.getTerm() &&
            systemOpTime.getTimestamp() >= _lastOp.getTimestamp()) {
            _lastOp = systemOpTime;
        } else {
            LOGV2(21280,
                  "Not setting the last OpTime for this Client from {lastOp} to the current system "
                  "time of {systemOpTime} as that would be moving the OpTime backwards.  This "
                  "should only happen if there was a rollback recently",
                  "Not setting the last OpTime for this Client to the current system time as that "
                  "would be moving the OpTime backwards. This should only happen if there was a "
                  "rollback recently",
                  "lastOp"_attr = _lastOp,
                  "systemOpTime"_attr = systemOpTime);
        }

        lastOpInfo(opCtx).lastOpSetExplicitly = true;

        // Throw if getLatestWriteOpTime failed.
        uassertStatusOK(status);
    }
}

void ReplClientInfo::setLastOpToSystemLastOpTimeIgnoringInterrupt(OperationContext* opCtx) {
    try {
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    } catch (const ExceptionForCat<ErrorCategory::Interruption>& e) {
        // In most cases, it is safe to ignore interruption errors because we cannot use the same
        // OperationContext to wait for writeConcern anyways.
        LOGV2_DEBUG(21281,
                    2,
                    "Ignoring set last op interruption error: {error}",
                    "Ignoring set last op interruption error",
                    "error"_attr = e.toStatus());
    }
}

}  // namespace repl
}  // namespace mongo

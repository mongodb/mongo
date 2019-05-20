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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_client_info.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/decorable.h"
#include "mongo/util/log.h"

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
        auto systemOpTime = replCoord->getMyLastAppliedOpTime();

        // If the system optime has gone backwards, that must mean that there was a rollback.
        // This is safe, but the last op for a Client should never go backwards, so just leave
        // the last op for this Client as it was.
        if (systemOpTime >= _lastOp) {
            _lastOp = systemOpTime;
        } else {
            log() << "Not setting the last OpTime for this Client from " << _lastOp
                  << " to the current system time of " << systemOpTime
                  << " as that would be moving the OpTime backwards.  This should only happen if "
                     "there was a rollback recently";
        }

        lastOpInfo(opCtx).lastOpSetExplicitly = true;
    }
}

}  // namespace repl
}  // namespace mongo

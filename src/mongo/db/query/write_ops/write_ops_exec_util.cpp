// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/write_ops_exec_util.h"

#include "mongo/logv2/log.h"

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
        //
        // Exceptions must not escape a destructor. Swallowing errors here is safe because the
        // write already completed successfully. Failing to advance lastOp only degrades
        // writeConcern acknowledgement for this one operation, which is preferable to crashing
        // the server. The primary case this guards against is transient resource exhaustion
        // such as WiredTiger session_max being reached under high connection load.
        try {
            replClientInfo().setLastOpToSystemLastOpTimeIgnoringCtxInterrupted(_opCtx);
            LOGV2_DEBUG(20888,
                        5,
                        "Set last op to system time",
                        "timestamp"_attr = replClientInfo().getLastOp().getTimestamp());
        } catch (const DBException& e) {
            LOGV2_WARNING(12810300,
                          "Failed to set last op to system time in LastOpFixer destructor",
                          "error"_attr = e.toStatus());
        }
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

}  // namespace mongo::write_ops_exec

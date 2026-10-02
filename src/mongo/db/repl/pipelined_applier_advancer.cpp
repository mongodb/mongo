// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_applier_advancer.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <exception>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo::repl {

PipelinedApplierAdvancer::PipelinedApplierAdvancer(PipelinedApplierBatchTracker& batchTracker,
                                                   PublishBatchFn publishBatch)
    : _batchTracker(batchTracker), _publishBatch(std::move(publishBatch)) {
    invariant(_publishBatch);
}

PipelinedApplierAdvancer::~PipelinedApplierAdvancer() {
    if (_thread.joinable()) {
        _batchTracker.onWorkerAbandonment();
    }
    shutdownAndJoin();
}

void PipelinedApplierAdvancer::startup() {
    invariant(!_thread.joinable());
    invariant(!_batchTracker.isShutdown());
    LOGV2(13469700, "Starting pipelined oplog applier advancer");
    _thread = stdx::thread([this] { _advancerLoop(); });
}

void PipelinedApplierAdvancer::shutdownAndJoin() {
    if (!_thread.joinable()) {
        return;
    }
    LOGV2(13469701, "Shutting down pipelined oplog applier advancer");
    // The dispatcher has stopped adding work; workers can still complete their queued slices.
    // Abandoned work stays unpublished; either outcome permits stopping the publication thread.
    // Explicitly discard the nodiscard result, as shutdown proceeds whether publication drained or
    // a worker abandoned its batch.
    static_cast<void>(_batchTracker.waitUntilIdle());
    _batchTracker.shutdown();
    _thread.join();
    LOGV2(13469702, "Pipelined oplog applier advancer shut down");
}

void PipelinedApplierAdvancer::_advancerLoop() {
    try {
        Client::initThread("PipelinedOplogApplierAdvancer",
                           getGlobalServiceContext()->getService(),
                           Client::noSession(),
                           ClientOperationKillableByStepdown{false});
        AuthorizationSession::get(cc())->grantInternalAuthorization();

        OpTime lastPublishedOpTime;
        while (true) {
            // Ready batches do not wait; 100ms is a backstop when the front batch is still being
            // worked on.
            // TODO SERVER-135581: Make the advancer wakeup interval a tunable server parameter.
            auto batch = _batchTracker.popCompletedBatch(Milliseconds(100));
            if (!batch) {
                if (_batchTracker.isShutdown()) {
                    return;
                }
                continue;
            }
            invariant(batch->lastOpTime.opTime > lastPublishedOpTime,
                      "Pipelined oplog applier publication must advance in oplog order");
            _publishBatch(*batch);
            lastPublishedOpTime = batch->lastOpTime.opTime;
            _batchTracker.onBatchPublished();
        }
    } catch (const DBException& ex) {
        LOGV2_FATAL(13469704, "Pipelined oplog applier advancer failed", "error"_attr = redact(ex));
    } catch (const std::exception& ex) {
        LOGV2_FATAL(13469705,
                    "Pipelined oplog applier advancer threw an exception",
                    "error"_attr = redact(ex.what()));
    } catch (...) {
        LOGV2_FATAL(13469706, "Pipelined oplog applier advancer threw an unknown exception");
    }
}

}  // namespace mongo::repl

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_flusher.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {

MONGO_FAIL_POINT_DEFINE(hangAfterSizeCountCheckpointCoordinatorStartupPublishesThreads);
MONGO_FAIL_POINT_DEFINE(hangAfterReplicatedFastCountSnapshot);
MONGO_FAIL_POINT_DEFINE(sleepAfterFlush);

SizeCountCheckpointCoordinator::SizeCountCheckpointCoordinator(
    SizeCountStore& sizeCountStore,
    SizeCountTimestampStore& timestampStore,
    UUID oplogUuid,
    Timestamp startCheckpointingAfterTS)
    : _sizeCountStore(sizeCountStore), _timestampStore(timestampStore), _buffer([&]() {
          const boost::optional<RecordId> lastBufferedRid =
              startCheckpointingAfterTS == Timestamp::min()
              ? boost::optional<RecordId>{}
              : massertStatusOK(
                    record_id_helpers::keyForOptime(startCheckpointingAfterTS, KeyFormat::Long));
          return SizeCountCheckpointBuffer(oplogUuid, lastBufferedRid);
      }()) {}

SizeCountCheckpointCoordinator::~SizeCountCheckpointCoordinator() {
    stdx::thread tailer;
    stdx::thread flusher;

    {
        std::lock_guard lk(_mutex);
        _shutdownRequested = true;

        tailer = std::move(_tailerThread);
        flusher = std::move(_flushThread);

        // Interrupt the worker opCtxs before joining. The destructor may run while the caller
        // holds RSTL_X (e.g. during stepdown via onStepDownHook). The tailer and flush threads may
        // race past their opCtx interrupt checks and block on RSTL_IS/IX acquisition, which will
        // never be granted while the caller holds RSTL_X, causing a deadlock. Interrupting the
        // opCtxs makes any pending lock wait throw immediately so the threads can exit.
        _opCtxGroup.interrupt(ErrorCodes::InterruptedDueToReplStateChange);
    }

    // join() throws an exception if it is called on a non-joinable thread. If startup() is not
    // called before this destructor, these threads are not joinable, and the destructor does
    // nothing. It is legal to call this destructor without calling startup(), e.g., during testing.
    if (tailer.joinable() != flusher.joinable()) {
        LOGV2_FATAL(13037102,
                    "Replicated fast count oplog tailer thread and flusher thread should have same "
                    "joinable status",
                    "Oplog tailer"_attr = tailer.joinable(),
                    "Flusher"_attr = flusher.joinable());
    }
    if (tailer.joinable() && flusher.joinable()) {
        tailer.join();
        flusher.join();
    }
}

void SizeCountCheckpointCoordinator::startup(ServiceContext* service) {
    {
        std::lock_guard lk(_mutex);
        invariant(!_shutdownRequested);
        tassert(13037100,
                "Replicated fast count oplog tailer thread already started",
                !_tailerThread.joinable());
        tassert(13037101,
                "Replicated fast count flusher thread already started",
                !_flushThread.joinable());

        LOGV2(13215700, "SizeCountCheckpointCoordinator spawning tailer and flusher threads");

        _tailerThread = stdx::thread([this, service] { _runTailerThread(service); });
        _flushThread = stdx::thread([this, service] { _runFlushThread(service); });
    }

    if (MONGO_unlikely(
            hangAfterSizeCountCheckpointCoordinatorStartupPublishesThreads.shouldFail())) {
        hangAfterSizeCountCheckpointCoordinatorStartupPublishesThreads.pauseWhileSet();
    }
}

bool SizeCountCheckpointCoordinator::isRunning_ForTest() const {
    std::lock_guard lk(_mutex);
    return _flushThread.joinable() && _tailerThread.joinable();
}

void SizeCountCheckpointCoordinator::requestFlush() {
    {
        std::lock_guard lk(_flushRequestMutex);
        _flushRequested = true;
    }
    _flushRequestCv.notify_one();
}

void SizeCountCheckpointCoordinator::flushSync_ForTest(OperationContext* opCtx) {
    oplog_tailer::bufferNewOplogEntries(opCtx, _buffer);
    if (auto batch = _buffer.checkoutForFlush()) {
        flusher::flush(opCtx, _sizeCountStore, _timestampStore, *batch);
        _buffer.acknowledgeFlush();
    }
}

bool SizeCountCheckpointCoordinator::isFlushRequested_ForTest() const {
    std::lock_guard lk(_flushRequestMutex);
    return _flushRequested;
}

void SizeCountCheckpointCoordinator::_runTailerThread(ServiceContext* service) {
    ThreadClient tc("Replicated fast count oplog tailer", service->getService());
    AuthorizationSession::get(cc())->grantInternalAuthorization();

    auto opCtxHolder = [&]() -> boost::optional<OperationContextGroup::Context> {
        std::lock_guard lk(_mutex);
        if (_shutdownRequested) {
            return boost::none;
        }
        return _opCtxGroup.makeOperationContext(cc());
    }();
    if (!opCtxHolder) {
        return;
    }

    ScopedAdmissionPriority<ExecutionAdmissionContext> skipTicketAcquisition(
        opCtxHolder->opCtx(), AdmissionContext::Priority::kExempt);

    // Run until no longer a primary.
    oplog_tailer::run(opCtxHolder->opCtx(), _buffer);
}

void SizeCountCheckpointCoordinator::_runFlushThread(ServiceContext* service) {
    ThreadClient tc("Replicated fast count flusher", service->getService());
    AuthorizationSession::get(cc())->grantInternalAuthorization();

    auto opCtxHolder = [&]() -> boost::optional<OperationContextGroup::Context> {
        std::lock_guard lk(_mutex);
        if (_shutdownRequested) {
            return boost::none;
        }
        return _opCtxGroup.makeOperationContext(cc());
    }();
    if (!opCtxHolder) {
        return;
    }

    ScopedAdmissionPriority<ExecutionAdmissionContext> skipTicketAcquisition(
        opCtxHolder->opCtx(), AdmissionContext::Priority::kExempt);

    OperationContext* opCtx = opCtxHolder->opCtx();

    // Run until no longer a primary.
    LOGV2(13215703, "Replicated fast count flusher thread started");
    setFlusherIsRunning(true);
    ON_BLOCK_EXIT([&] {
        setFlusherIsRunning(false);
        LOGV2(13215704, "Replicated fast count flusher thread exiting");
        std::lock_guard lk(_flushRequestMutex);
        _flushRequested = false;
    });

    while (true) {
        try {
            {
                std::unique_lock lk(_flushRequestMutex);
                opCtx->waitForConditionOrInterrupt(
                    _flushRequestCv, lk, [this] { return _flushRequested; });
                _flushRequested = false;
            }

            const Date_t flushStart = Date_t::now();
            boost::optional<flusher::FlushResult> result = boost::none;
            const auto batch = _buffer.checkoutForFlush();
            hangAfterReplicatedFastCountSnapshot.pauseWhileSet();
            if (batch.has_value()) {
                result = flusher::flush(opCtx, _sizeCountStore, _timestampStore, *batch);
                // Acknowledge the flush in the SizeCountCheckpointBuffer, regardless if data was
                // persisted or the flush was a no-op. The only time we do not acknowledge a flush
                // is if flush() throws an exception.
                _buffer.acknowledgeFlush();
            }

            sleepAfterFlush.execute([](const BSONObj& data) {
                if (auto elem = data["sleepMs"]; elem) {
                    sleepmillis(elem.numberInt());
                }
            });

            const Milliseconds duration = Date_t::now() - flushStart;
            if (result.has_value()) {
                LOGV2(13195100,
                      "SizeCountCheckpointFlusher flushed successfully",
                      "previousValidAsOfTS"_attr = result->previousValidAsOfTS,
                      "newValidAsOfTS"_attr = result->newValidAsOfTS,
                      "checkpointBufferSize"_attr = result->checkpointBufferSize,
                      "entryWriteCount"_attr = result->entryWriteCount,
                      "flushAttempts"_attr = result->flushAttempts,
                      "duration"_attr = duration);
                recordFlush(flushStart, result->entryWriteCount);
            } else {
                LOGV2_DEBUG(13195101,
                            3,
                            "SizeCountCheckpointFlusher did not flush",
                            "duration"_attr = duration);
            }
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::InterruptedDueToReplStateChange ||
                ErrorCodes::isShutdownError(ex.code())) {
                // The flusher is a primary-only thread. Stop the thread when stepping down or
                // shutting down.
                LOGV2(12917804,
                      "Replicated fast count flusher interrupted",
                      "error"_attr = ex.toStatus());
                return;
            }
            incrementFlushFailureCount();
            LOGV2_WARNING(12917805,
                          "Exception handled in replicated fast count flusher flush loop",
                          "error"_attr = ex.toStatus());
        }
    }
}

}  // namespace mongo::replicated_fast_count

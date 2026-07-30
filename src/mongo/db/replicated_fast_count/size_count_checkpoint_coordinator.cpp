// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_coordinator.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {

MONGO_FAIL_POINT_DEFINE(hangAfterSizeCountCheckpointCoordinatorStartupPublishesThreads);

SizeCountCheckpointCoordinator::SizeCountCheckpointCoordinator(
    SizeCountStore& sizeCountStore,
    SizeCountTimestampStore& timestampStore,
    UUID oplogUuid,
    Timestamp startCheckpointingAfterTS)
    : _flusher(std::make_unique<SizeCountCheckpointFlusher>(&sizeCountStore, &timestampStore)),
      _buffer([&]() {
          const boost::optional<RecordId> lastBufferedRid =
              startCheckpointingAfterTS == Timestamp::min()
              ? boost::optional<RecordId>{}
              : massertStatusOK(
                    record_id_helpers::keyForOptime(startCheckpointingAfterTS, KeyFormat::Long));
          return std::make_unique<SizeCountCheckpointBuffer>(oplogUuid, lastBufferedRid);
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
                    "SizeCountCheckpointOplogTailer thread and SizeCountCheckpointFlusher thread "
                    "should have same joinable status",
                    "SizeCountCheckpointOplogTailer"_attr = tailer.joinable(),
                    "SizeCountCheckpointFlusher"_attr = flusher.joinable());
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
                "SizeCountCheckpointOplogTailer thread already started",
                !_tailerThread.joinable());
        tassert(13037101,
                "SizeCountCheckpointFlusher thread already started",
                !_flushThread.joinable());

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
    _flusher->requestFlush();
}

void SizeCountCheckpointCoordinator::flushSync_ForTest(OperationContext* opCtx) {
    oplog_tailer::bufferNewOplogEntries(opCtx, *_buffer);
    _flusher->runOneFlushCycle_ForTest(opCtx, *_buffer);
}

bool SizeCountCheckpointCoordinator::isFlushRequested_ForTest() const {
    return _flusher->isFlushRequested_ForTest();
}

void SizeCountCheckpointCoordinator::_runTailerThread(ServiceContext* service) {
    ThreadClient tc("SizeCountCheckpointOplogTailer", service->getService());
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
    oplog_tailer::run(opCtxHolder->opCtx(), *_buffer);
}

void SizeCountCheckpointCoordinator::_runFlushThread(ServiceContext* service) {
    ThreadClient tc("SizeCountCheckpointFlusher", service->getService());
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
    _flusher->run(opCtxHolder->opCtx(), *_buffer);
}

}  // namespace mongo::replicated_fast_count

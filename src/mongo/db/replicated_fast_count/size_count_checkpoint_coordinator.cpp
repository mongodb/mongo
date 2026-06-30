/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
MONGO_FAIL_POINT_DEFINE(hangBeforeSizeCountCheckpointCoordinatorShutdownJoins);

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
      }()),
      _sizeCountStore(sizeCountStore),
      _timestampStore(timestampStore) {}

SizeCountCheckpointCoordinator::~SizeCountCheckpointCoordinator() {
    shutdown();
}
void SizeCountCheckpointCoordinator::startup(ServiceContext* service) {
    {
        std::lock_guard lk(_mutex);
        if (_started || _shutdownRequested) {
            return;
        }
        _started = true;

        _tailerThread = stdx::thread([this, service] { _runTailerThread(service); });
        _flushThread = stdx::thread([this, service] { _runFlushThread(service); });
    }

    if (MONGO_unlikely(
            hangAfterSizeCountCheckpointCoordinatorStartupPublishesThreads.shouldFail())) {
        hangAfterSizeCountCheckpointCoordinatorStartupPublishesThreads.pauseWhileSet();
    }
}

void SizeCountCheckpointCoordinator::shutdown() {
    stdx::thread tailer;
    stdx::thread flusher;

    {
        std::lock_guard lk(_mutex);
        if (_shutdownRequested) {
            return;
        }
        _shutdownRequested = true;

        tailer = std::move(_tailerThread);
        flusher = std::move(_flushThread);

        // Interrupt the worker opCtxs before joining. shutdown() may be called while the caller
        // holds RSTL_X (e.g. during stepdown via onStepDownHook). The tailer and flush threads may
        // race past their opCtx interrupt checks and block on RSTL_IS/IX acquisition, which will
        // never be granted while the caller holds RSTL_X, causing a deadlock. Interrupting the
        // opCtxs makes any pending lock wait throw immediately so the threads can exit.
        _opCtxGroup.interrupt(ErrorCodes::InterruptedDueToReplStateChange);
    }

    hangBeforeSizeCountCheckpointCoordinatorShutdownJoins.pauseWhileSet();

    if (tailer.joinable()) {
        tailer.join();
    }
    if (flusher.joinable()) {
        flusher.join();
    }
}

bool SizeCountCheckpointCoordinator::isRunning_ForTest() const {
    std::lock_guard lk(_mutex);
    return _started && !_shutdownRequested;
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

SizeCountCheckpointBuffer* SizeCountCheckpointCoordinator::getBuffer_ForTest() const {
    std::lock_guard lk(_mutex);
    return _buffer.get();
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

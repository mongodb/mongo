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

#include "mongo/platform/basic.h"

#include "mongo/db/s/active_migrations_registry.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace {

const auto getRegistry = ServiceContext::declareDecoration<ActiveMigrationsRegistry>();

}  // namespace

ActiveMigrationsRegistry::ActiveMigrationsRegistry() = default;

ActiveMigrationsRegistry::~ActiveMigrationsRegistry() {
    invariant(!_activeMoveChunkState);
}

ActiveMigrationsRegistry& ActiveMigrationsRegistry::get(ServiceContext* service) {
    return getRegistry(service);
}

ActiveMigrationsRegistry& ActiveMigrationsRegistry::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

StatusWith<ScopedDonateChunk> ActiveMigrationsRegistry::registerDonateChunk(
    const MoveChunkRequest& args) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_activeReceiveChunkState) {
        return _activeReceiveChunkState->constructErrorStatus();
    }

    if (_activeMoveChunkState) {
        if (_activeMoveChunkState->args == args) {
            return {ScopedDonateChunk(nullptr, false, _activeMoveChunkState->notification)};
        }

        return _activeMoveChunkState->constructErrorStatus();
    }

    _activeMoveChunkState.emplace(args);

    return {ScopedDonateChunk(this, true, _activeMoveChunkState->notification)};
}

StatusWith<ScopedReceiveChunk> ActiveMigrationsRegistry::registerReceiveChunk(
    const NamespaceString& nss, const ChunkRange& chunkRange, const ShardId& fromShardId) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_activeReceiveChunkState) {
        return _activeReceiveChunkState->constructErrorStatus();
    }

    if (_activeMoveChunkState) {
        return _activeMoveChunkState->constructErrorStatus();
    }

    _activeReceiveChunkState.emplace(nss, chunkRange, fromShardId);

    return {ScopedReceiveChunk(this)};
}

StatusWith<ScopedSplitMergeChunk> ActiveMigrationsRegistry::registerSplitOrMergeChunk(
    OperationContext* opCtx, const NamespaceString& nss, const ChunkRange& chunkRange) {
    stdx::unique_lock<Latch> ul(_mutex);

    // In order for splits to not block for too long behind a potential chunk migration, limit the
    // duration of waiting for conflicting operations to at most 5 seconds. Otherwise, due to the
    // fact that chunk splits block write operations on the MongoS it is possible that the write
    // workload gets a really long stall.
    const auto deadline = opCtx->getServiceContext()->getFastClockSource()->now() + Seconds{5};
    if (!opCtx->waitForConditionOrInterruptUntil(_chunkOperationsStateChangedCV, ul, deadline, [&] {
            return !(_activeMoveChunkState && _activeMoveChunkState->args.getNss() == nss) &&
                !_activeSplitMergeChunkStates.count(nss);
        })) {
        return {ErrorCodes::LockBusy, "Timed out waiting for concurrent migration to complete"};
    }

    auto [it, inserted] =
        _activeSplitMergeChunkStates.emplace(nss, ActiveSplitMergeChunkState(nss, chunkRange));
    invariant(inserted);

    return {ScopedSplitMergeChunk(this, nss)};
}

boost::optional<NamespaceString> ActiveMigrationsRegistry::getActiveDonateChunkNss() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_activeMoveChunkState) {
        return _activeMoveChunkState->args.getNss();
    }

    return boost::none;
}

BSONObj ActiveMigrationsRegistry::getActiveMigrationStatusReport(OperationContext* opCtx) {
    boost::optional<NamespaceString> nss;
    {
        stdx::lock_guard<Latch> lk(_mutex);

        if (_activeMoveChunkState) {
            nss = _activeMoveChunkState->args.getNss();
        }
    }

    // The state of the MigrationSourceManager could change between taking and releasing the mutex
    // above and then taking the collection lock here, but that's fine because it isn't important to
    // return information on a migration that just ended or started. This is just best effort and
    // desireable for reporting, and then diagnosing, migrations that are stuck.
    if (nss) {
        // Lock the collection so nothing changes while we're getting the migration report.
        AutoGetCollection autoColl(opCtx, nss.get(), MODE_IS);
        auto csr = CollectionShardingRuntime::get(opCtx, nss.get());
        auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);

        if (auto msm = MigrationSourceManager::get(csr, csrLock)) {
            return msm->getMigrationStatusReport();
        }
    }

    return BSONObj();
}

void ActiveMigrationsRegistry::_clearDonateChunk() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_activeMoveChunkState);
    _activeMoveChunkState.reset();
    _chunkOperationsStateChangedCV.notify_all();
}

void ActiveMigrationsRegistry::_clearReceiveChunk() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_activeReceiveChunkState);
    _activeReceiveChunkState.reset();
    _chunkOperationsStateChangedCV.notify_all();
}

void ActiveMigrationsRegistry::_clearSplitMergeChunk(const NamespaceString& nss) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_activeSplitMergeChunkStates.erase(nss));
    _chunkOperationsStateChangedCV.notify_all();
}

Status ActiveMigrationsRegistry::ActiveMoveChunkState::constructErrorStatus() const {
    return {ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Unable to start new balancer operation because this shard is "
                             "currently donating chunk "
                          << ChunkRange(args.getMinKey(), args.getMaxKey()).toString()
                          << " for namespace " << args.getNss().ns() << " to "
                          << args.getToShardId()};
}

Status ActiveMigrationsRegistry::ActiveReceiveChunkState::constructErrorStatus() const {
    return {ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Unable to start new balancer operation because this shard is "
                             "currently receiving chunk "
                          << range.toString() << " for namespace " << nss.ns() << " from "
                          << fromShardId};
}

ScopedDonateChunk::ScopedDonateChunk(ActiveMigrationsRegistry* registry,
                                     bool shouldExecute,
                                     std::shared_ptr<Notification<Status>> completionNotification)
    : _registry(registry),
      _shouldExecute(shouldExecute),
      _completionNotification(std::move(completionNotification)) {}

ScopedDonateChunk::~ScopedDonateChunk() {
    if (_registry && _shouldExecute) {
        // If this is a newly started migration the caller must always signal on completion
        invariant(*_completionNotification);
        _registry->_clearDonateChunk();
    }
}

ScopedDonateChunk::ScopedDonateChunk(ScopedDonateChunk&& other) {
    *this = std::move(other);
}

ScopedDonateChunk& ScopedDonateChunk::operator=(ScopedDonateChunk&& other) {
    if (&other != this) {
        _registry = other._registry;
        other._registry = nullptr;
        _shouldExecute = other._shouldExecute;
        _completionNotification = std::move(other._completionNotification);
    }

    return *this;
}

void ScopedDonateChunk::signalComplete(Status status) {
    invariant(_shouldExecute);
    _completionNotification->set(status);
}

Status ScopedDonateChunk::waitForCompletion(OperationContext* opCtx) {
    invariant(!_shouldExecute);
    return _completionNotification->get(opCtx);
}

ScopedReceiveChunk::ScopedReceiveChunk(ActiveMigrationsRegistry* registry) : _registry(registry) {}

ScopedReceiveChunk::~ScopedReceiveChunk() {
    if (_registry) {
        _registry->_clearReceiveChunk();
    }
}

ScopedReceiveChunk::ScopedReceiveChunk(ScopedReceiveChunk&& other) {
    *this = std::move(other);
}

ScopedReceiveChunk& ScopedReceiveChunk::operator=(ScopedReceiveChunk&& other) {
    if (&other != this) {
        _registry = other._registry;
        other._registry = nullptr;
    }

    return *this;
}

ScopedSplitMergeChunk::ScopedSplitMergeChunk(ActiveMigrationsRegistry* registry,
                                             const NamespaceString& nss)
    : _registry(registry), _nss(nss) {}

ScopedSplitMergeChunk::~ScopedSplitMergeChunk() {
    if (_registry) {
        _registry->_clearSplitMergeChunk(_nss);
    }
}

ScopedSplitMergeChunk::ScopedSplitMergeChunk(ScopedSplitMergeChunk&& other) {
    *this = std::move(other);
}

ScopedSplitMergeChunk& ScopedSplitMergeChunk::operator=(ScopedSplitMergeChunk&& other) {
    if (&other != this) {
        _registry = other._registry;
        other._registry = nullptr;
        _nss = std::move(other._nss);
    }

    return *this;
}

}  // namespace mongo

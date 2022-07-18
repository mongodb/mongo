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
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration


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

void ActiveMigrationsRegistry::lock(OperationContext* opCtx, StringData reason) {
    // The method requires the requesting operation to be interruptible
    invariant(opCtx->shouldAlwaysInterruptAtStepDownOrUp());
    stdx::unique_lock<Latch> lock(_mutex);

    // This wait is to hold back additional lock requests while there is already one in progress
    opCtx->waitForConditionOrInterrupt(
        _chunkOperationsStateChangedCV, lock, [this] { return !_migrationsBlocked; });

    // Setting flag before condvar returns to block new migrations from starting. (Favoring writers)
    LOGV2(467560, "Going to start blocking migrations", "reason"_attr = reason);
    _migrationsBlocked = true;

    ScopeGuard unblockMigrationsOnError([&] { _migrationsBlocked = false; });

    // Wait for any ongoing chunk modifications to complete
    opCtx->waitForConditionOrInterrupt(_chunkOperationsStateChangedCV, lock, [this] {
        return !(_activeMoveChunkState || _activeReceiveChunkState);
    });

    // lock() may be called while the node is still completing its draining mode; if so, reject the
    // request with a retriable error and allow the draining mode to invoke registerReceiveChunk()
    // as part of its recovery sequence.
    {
        AutoGetDb autoDB(opCtx, NamespaceString::kAdminDb, MODE_IS);
        uassert(ErrorCodes::NotWritablePrimary,
                "Cannot lock the registry while the node is in draining mode",
                repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(
                    opCtx, NamespaceString::kAdminDb));
    }

    unblockMigrationsOnError.dismiss();
}

void ActiveMigrationsRegistry::unlock(StringData reason) {
    stdx::lock_guard<Latch> lock(_mutex);

    LOGV2(467561, "Going to stop blocking migrations", "reason"_attr = reason);
    _migrationsBlocked = false;

    _chunkOperationsStateChangedCV.notify_all();
}

StatusWith<ScopedDonateChunk> ActiveMigrationsRegistry::registerDonateChunk(
    OperationContext* opCtx, const ShardsvrMoveRange& args) {
    stdx::unique_lock<Latch> ul(_mutex);

    opCtx->waitForConditionOrInterrupt(_chunkOperationsStateChangedCV, ul, [&] {
        return !_activeSplitMergeChunkStates.count(args.getCommandParameter());
    });

    if (_activeReceiveChunkState) {
        return _activeReceiveChunkState->constructErrorStatus();
    }

    if (_activeMoveChunkState) {
        auto activeMoveChunkStateBSON = _activeMoveChunkState->args.toBSON({});

        if (activeMoveChunkStateBSON.woCompare(args.toBSON({})) == 0) {
            LOGV2(6386800,
                  "Registering new chunk donation",
                  logAttrs(args.getCommandParameter()),
                  "min"_attr = args.getMin(),
                  "max"_attr = args.getMax(),
                  "toShardId"_attr = args.getToShard());
            return {ScopedDonateChunk(nullptr, false, _activeMoveChunkState->notification)};
        }

        LOGV2(6386801,
              "Rejecting donate chunk due to conflicting migration in progress",
              logAttrs(args.getCommandParameter()),
              "runningMigration"_attr = activeMoveChunkStateBSON,
              "requestedMigration"_attr = args.toBSON({}));

        return _activeMoveChunkState->constructErrorStatus();
    }

    if (_migrationsBlocked) {
        return {ErrorCodes::ConflictingOperationInProgress,
                "Unable to start new balancer operation because the ActiveMigrationsRegistry of "
                "this shard is temporarily locked"};
    }

    _activeMoveChunkState.emplace(args);

    return {ScopedDonateChunk(this, true, _activeMoveChunkState->notification)};
}

StatusWith<ScopedReceiveChunk> ActiveMigrationsRegistry::registerReceiveChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkRange& chunkRange,
    const ShardId& fromShardId,
    bool waitForCompletionOfConflictingOps) {
    stdx::unique_lock<Latch> ul(_mutex);

    if (waitForCompletionOfConflictingOps) {
        opCtx->waitForConditionOrInterrupt(_chunkOperationsStateChangedCV, ul, [this] {
            return !_migrationsBlocked && !_activeMoveChunkState && !_activeReceiveChunkState;
        });
    } else {
        if (_activeReceiveChunkState) {
            return _activeReceiveChunkState->constructErrorStatus();
        }

        if (_activeMoveChunkState) {
            LOGV2(6386802,
                  "Rejecting receive chunk due to conflicting donate chunk in progress",
                  logAttrs(_activeMoveChunkState->args.getCommandParameter()),
                  "runningMigration"_attr = _activeMoveChunkState->args.toBSON({}));
            return _activeMoveChunkState->constructErrorStatus();
        }

        if (_migrationsBlocked) {
            return {
                ErrorCodes::ConflictingOperationInProgress,
                "Unable to start new balancer operation because the ActiveMigrationsRegistry of "
                "this shard is temporarily locked"};
        }
    }

    _activeReceiveChunkState.emplace(nss, chunkRange, fromShardId);
    return {ScopedReceiveChunk(this)};
}

StatusWith<ScopedSplitMergeChunk> ActiveMigrationsRegistry::registerSplitOrMergeChunk(
    OperationContext* opCtx, const NamespaceString& nss, const ChunkRange& chunkRange) {
    stdx::unique_lock<Latch> ul(_mutex);

    opCtx->waitForConditionOrInterrupt(_chunkOperationsStateChangedCV, ul, [&] {
        return !(_activeMoveChunkState &&
                 _activeMoveChunkState->args.getCommandParameter() == nss) &&
            !_activeSplitMergeChunkStates.count(nss);
    });

    auto [it, inserted] =
        _activeSplitMergeChunkStates.emplace(nss, ActiveSplitMergeChunkState(nss, chunkRange));
    invariant(inserted);

    return {ScopedSplitMergeChunk(this, nss)};
}

boost::optional<NamespaceString> ActiveMigrationsRegistry::getActiveDonateChunkNss() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_activeMoveChunkState) {
        return _activeMoveChunkState->args.getCommandParameter();
    }

    return boost::none;
}

BSONObj ActiveMigrationsRegistry::getActiveMigrationStatusReport(OperationContext* opCtx) {
    boost::optional<NamespaceString> nss;
    {
        stdx::lock_guard<Latch> lk(_mutex);

        if (_activeMoveChunkState) {
            nss = _activeMoveChunkState->args.getCommandParameter();
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
    LOGV2(6386803,
          "Unregistering donate chunk",
          logAttrs(_activeMoveChunkState->args.getCommandParameter()),
          "min"_attr = _activeMoveChunkState->args.getMin().get_value_or(BSONObj()),
          "max"_attr = _activeMoveChunkState->args.getMax().get_value_or(BSONObj()),
          "toShardId"_attr = _activeMoveChunkState->args.getToShard());
    _activeMoveChunkState.reset();
    _chunkOperationsStateChangedCV.notify_all();
}

void ActiveMigrationsRegistry::_clearReceiveChunk() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_activeReceiveChunkState);
    LOGV2(5004703,
          "clearReceiveChunk",
          "currentKeys"_attr = ChunkRange(_activeReceiveChunkState->range.getMin(),
                                          _activeReceiveChunkState->range.getMax())
                                   .toString());
    _activeReceiveChunkState.reset();
    _chunkOperationsStateChangedCV.notify_all();
}

void ActiveMigrationsRegistry::_clearSplitMergeChunk(const NamespaceString& nss) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_activeSplitMergeChunkStates.erase(nss));
    _chunkOperationsStateChangedCV.notify_all();
}

Status ActiveMigrationsRegistry::ActiveMoveChunkState::constructErrorStatus() const {
    std::string errMsg = fmt::format(
        "Unable to start new balancer operation because this shard is currently donating range "
        "'{}{}' for namespace {} to shard {}",
        (args.getMin() ? "min: " + args.getMin()->toString() + " - " : ""),
        (args.getMax() ? "max: " + args.getMax()->toString() : ""),
        args.getCommandParameter().ns(),
        args.getToShard().toString());
    return {ErrorCodes::ConflictingOperationInProgress, std::move(errMsg)};
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
        // If this is a newly started migration the outcome must have been set by the holder
        invariant(_completionOutcome);
        _registry->_clearDonateChunk();
        _completionNotification->set(*_completionOutcome);
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
    invariant(!_completionOutcome.has_value());
    _completionOutcome = status;
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

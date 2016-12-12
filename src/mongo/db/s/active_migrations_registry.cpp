/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/active_migrations_registry.h"

#include "mongo/base/status_with.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/util/assert_util.h"

namespace mongo {

ActiveMigrationsRegistry::ActiveMigrationsRegistry() = default;

ActiveMigrationsRegistry::~ActiveMigrationsRegistry() {
    invariant(!_activeMoveChunkState);
}

StatusWith<ScopedRegisterDonateChunk> ActiveMigrationsRegistry::registerDonateChunk(
    const MoveChunkRequest& args) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_activeReceiveChunkState) {
        return _activeReceiveChunkState->constructErrorStatus();
    }

    if (_activeMoveChunkState) {
        if (_activeMoveChunkState->args == args) {
            return {ScopedRegisterDonateChunk(nullptr, false, _activeMoveChunkState->notification)};
        }

        return _activeMoveChunkState->constructErrorStatus();
    }

    _activeMoveChunkState.emplace(args);

    return {ScopedRegisterDonateChunk(this, true, _activeMoveChunkState->notification)};
}

StatusWith<ScopedRegisterReceiveChunk> ActiveMigrationsRegistry::registerReceiveChunk(
    const NamespaceString& nss, const ChunkRange& chunkRange, const ShardId& fromShardId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_activeReceiveChunkState) {
        return _activeReceiveChunkState->constructErrorStatus();
    }

    if (_activeMoveChunkState) {
        return _activeMoveChunkState->constructErrorStatus();
    }

    _activeReceiveChunkState.emplace(nss, chunkRange, fromShardId);

    return {ScopedRegisterReceiveChunk(this)};
}

boost::optional<NamespaceString> ActiveMigrationsRegistry::getActiveDonateChunkNss() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_activeMoveChunkState) {
        return _activeMoveChunkState->args.getNss();
    }

    return boost::none;
}

BSONObj ActiveMigrationsRegistry::getActiveMigrationStatusReport(OperationContext* txn) {
    boost::optional<NamespaceString> nss;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

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
        AutoGetCollection autoColl(txn, nss.get(), MODE_IS);

        auto css = CollectionShardingState::get(txn, nss.get());
        if (css && css->getMigrationSourceManager()) {
            return css->getMigrationSourceManager()->getMigrationStatusReport();
        }
    }

    return BSONObj();
}

void ActiveMigrationsRegistry::_clearDonateChunk() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_activeMoveChunkState);
    _activeMoveChunkState.reset();
}

void ActiveMigrationsRegistry::_clearReceiveChunk() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_activeReceiveChunkState);
    _activeReceiveChunkState.reset();
}

Status ActiveMigrationsRegistry::ActiveMoveChunkState::constructErrorStatus() const {
    return {ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Unable to start new migration because this shard is currently "
                             "donating chunk "
                          << ChunkRange(args.getMinKey(), args.getMaxKey()).toString()
                          << " for namespace "
                          << args.getNss().ns()
                          << " to "
                          << args.getToShardId()};
}

Status ActiveMigrationsRegistry::ActiveReceiveChunkState::constructErrorStatus() const {
    return {ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Unable to start new migration because this shard is currently "
                             "receiving chunk "
                          << range.toString()
                          << " for namespace "
                          << nss.ns()
                          << " from "
                          << fromShardId};
}

ScopedRegisterDonateChunk::ScopedRegisterDonateChunk(
    ActiveMigrationsRegistry* registry,
    bool forUnregister,
    std::shared_ptr<Notification<Status>> completionNotification)
    : _registry(registry),
      _forUnregister(forUnregister),
      _completionNotification(std::move(completionNotification)) {}

ScopedRegisterDonateChunk::~ScopedRegisterDonateChunk() {
    if (_registry && _forUnregister) {
        // If this is a newly started migration the caller must always signal on completion
        invariant(*_completionNotification);
        _registry->_clearDonateChunk();
    }
}

ScopedRegisterDonateChunk::ScopedRegisterDonateChunk(ScopedRegisterDonateChunk&& other) {
    *this = std::move(other);
}

ScopedRegisterDonateChunk& ScopedRegisterDonateChunk::operator=(ScopedRegisterDonateChunk&& other) {
    if (&other != this) {
        _registry = other._registry;
        other._registry = nullptr;
        _forUnregister = other._forUnregister;
        _completionNotification = std::move(other._completionNotification);
    }

    return *this;
}

void ScopedRegisterDonateChunk::complete(Status status) {
    invariant(_forUnregister);
    _completionNotification->set(status);
}

Status ScopedRegisterDonateChunk::waitForCompletion(OperationContext* txn) {
    invariant(!_forUnregister);
    return _completionNotification->get(txn);
}

ScopedRegisterReceiveChunk::ScopedRegisterReceiveChunk(ActiveMigrationsRegistry* registry)
    : _registry(registry) {}

ScopedRegisterReceiveChunk::~ScopedRegisterReceiveChunk() {
    if (_registry) {
        _registry->_clearReceiveChunk();
    }
}

ScopedRegisterReceiveChunk::ScopedRegisterReceiveChunk(ScopedRegisterReceiveChunk&& other) {
    *this = std::move(other);
}

ScopedRegisterReceiveChunk& ScopedRegisterReceiveChunk::operator=(
    ScopedRegisterReceiveChunk&& other) {
    if (&other != this) {
        _registry = other._registry;
        other._registry = nullptr;
    }

    return *this;
}

}  // namespace mongo

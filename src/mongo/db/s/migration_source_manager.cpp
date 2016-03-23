/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_source_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

// Wait at most this much time for the recipient to catch up sufficiently so critical section can be
// entered
const Hours kMaxWaitToEnterCriticalSectionTimeout(6);

}  // namespace

MONGO_FP_DECLARE(failMigrationCommit);
MONGO_FP_DECLARE(hangBeforeCommitMigration);
MONGO_FP_DECLARE(hangBeforeLeavingCriticalSection);

MigrationSourceManager::MigrationSourceManager(OperationContext* txn, MoveChunkRequest request)
    : _args(std::move(request)), _startTime() {
    invariant(!txn->lockState()->isLocked());

    const auto& oss = OperationShardingState::get(txn);
    if (!oss.hasShardVersion()) {
        uasserted(ErrorCodes::InvalidOptions, "shard version is missing");
    }

    const ChunkVersion expectedCollectionVersion = oss.getShardVersion(_args.getNss());

    log() << "Starting chunk migration for [" << _args.getMinKey() << ", " << _args.getMaxKey()
          << ") with expected collection version " << expectedCollectionVersion;

    // Now that the collection is locked, snapshot the metadata and fetch the latest versions
    ShardingState* const shardingState = ShardingState::get(txn);

    ChunkVersion shardVersion;

    Status refreshStatus =
        shardingState->refreshMetadataNow(txn, _args.getNss().ns(), &shardVersion);
    if (!refreshStatus.isOK()) {
        uasserted(refreshStatus.code(),
                  str::stream() << "moveChunk cannot start migrate of chunk "
                                << "[" << _args.getMinKey() << "," << _args.getMaxKey()
                                << ") due to " << refreshStatus.toString());
    }

    if (shardVersion.majorVersion() == 0) {
        // If the major version is zero, this means we do not have any chunks locally to migrate in
        // the first place
        uasserted(ErrorCodes::IncompatibleShardingMetadata,
                  str::stream() << "moveChunk cannot start migrate of chunk "
                                << "[" << _args.getMinKey() << "," << _args.getMaxKey() << ")"
                                << " with zero shard version");
    }

    if (expectedCollectionVersion.epoch() != shardVersion.epoch()) {
        throw SendStaleConfigException(
            _args.getNss().ns(),
            str::stream() << "moveChunk cannot move chunk "
                          << "[" << _args.getMinKey() << "," << _args.getMaxKey() << "), "
                          << "collection may have been dropped. "
                          << "current epoch: " << shardVersion.epoch()
                          << ", cmd epoch: " << expectedCollectionVersion.epoch(),
            expectedCollectionVersion,
            shardVersion);
    }

    // Snapshot the committed metadata from the time the migration starts
    {
        ScopedTransaction scopedXact(txn, MODE_IS);
        AutoGetCollection autoColl(txn, _args.getNss(), MODE_IS);

        auto css = CollectionShardingState::get(txn, _args.getNss());
        _committedMetadata = css->getMetadata();
    }

    // With nonzero shard version, we must have a coll version >= our shard version
    invariant(_committedMetadata->getCollVersion() >= shardVersion);

    // With nonzero shard version, we must have a shard key
    invariant(!_committedMetadata->getKeyPattern().isEmpty());

    ChunkType origChunk;

    if (!_committedMetadata->getNextChunk(_args.getMinKey(), &origChunk) ||
        origChunk.getMin().woCompare(_args.getMinKey()) ||
        origChunk.getMax().woCompare(_args.getMaxKey())) {
        // Our boundaries are different from those passed in
        throw SendStaleConfigException(
            _args.getNss().ns(),
            str::stream() << "moveChunk cannot find chunk "
                          << "[" << _args.getMinKey() << "," << _args.getMaxKey() << ")"
                          << " to migrate, the chunk boundaries may be stale",
            expectedCollectionVersion,
            shardVersion);
    }
}

MigrationSourceManager::~MigrationSourceManager() {
    invariant(!_cloneDriver);
    invariant(!_uncommittedMetadata);
}

NamespaceString MigrationSourceManager::getNss() const {
    return _args.getNss();
}

Status MigrationSourceManager::startClone(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    invariant(_state == kCreated);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(txn); });

    grid.catalogManager(txn)
        ->logChange(txn,
                    "moveChunk.start",
                    _args.getNss().ns(),
                    BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                               << _args.getFromShardId() << "to" << _args.getToShardId()));

    _cloneDriver = stdx::make_unique<MigrationChunkClonerSourceLegacy>(
        _args, _committedMetadata->getKeyPattern());

    {
        // Register for notifications from the replication subsystem
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, _args.getNss(), MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(txn, _args.getNss().ns());
        css->setMigrationSourceManager(txn, this);
    }

    Status startCloneStatus = _cloneDriver->startClone(txn);
    if (!startCloneStatus.isOK()) {
        return startCloneStatus;
    }

    _state = kCloning;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::awaitToCatchUp(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    invariant(_state == kCloning);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(txn); });

    // Block until the cloner deems it appropriate to enter the critical section.
    Status catchUpStatus = _cloneDriver->awaitUntilCriticalSectionIsAppropriate(
        txn, kMaxWaitToEnterCriticalSectionTimeout);
    if (!catchUpStatus.isOK()) {
        return catchUpStatus;
    }

    _state = kCloneCaughtUp;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::enterCriticalSection(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    invariant(_state == kCloneCaughtUp);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(txn); });

    // Mark the shard as running critical operation, which requires recovery on crash
    Status status = ShardingStateRecovery::startMetadataOp(txn);
    if (!status.isOK()) {
        return status;
    }

    {
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, _args.getNss(), MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(txn, _args.getNss().ns());

        if (!css->getMetadata() ||
            !css->getMetadata()->getCollVersion().equals(_committedMetadata->getCollVersion())) {
            return {ErrorCodes::IncompatibleShardingMetadata,
                    str::stream()
                        << "Sharding metadata changed while holding distributed lock. Expected: "
                        << _committedMetadata->getCollVersion().toString()
                        << ", actual: " << css->getMetadata()->getCollVersion().toString()};
        }

        ChunkVersion uncommittedCollVersion = _committedMetadata->getCollVersion();
        uncommittedCollVersion.incMajor();

        // Bump the metadata's version up and "forget" about the chunk being moved. This is not the
        // commit point, but in practice the state in this shard won't change until the commit it
        // done.
        ChunkType chunk;
        chunk.setMin(_args.getMinKey());
        chunk.setMax(_args.getMaxKey());

        _uncommittedMetadata = _committedMetadata->cloneMigrate(chunk, uncommittedCollVersion);

        // IMPORTANT: After this line, the critical section is in place and needs to be rolled back
        // if anything fails, which would prevent commit to the config servers.
        _critSec = std::make_shared<CriticalSectionState>();
        css->setMetadata(_uncommittedMetadata);
    }

    log() << "Successfully entered critical section. Uncommited version: "
          << _uncommittedMetadata->getCollVersion();

    _state = kCriticalSection;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::commitDonateChunk(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    invariant(_state == kCriticalSection);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(txn); });

    // Tell the recipient shard to fetch the latest changes
    Status commitCloneStatus = _cloneDriver->commitClone(txn);

    if (MONGO_FAIL_POINT(failMigrationCommit) && commitCloneStatus.isOK()) {
        commitCloneStatus = {ErrorCodes::InternalError,
                             "Failing _recvChunkCommit due to failpoint."};
    }

    if (!commitCloneStatus.isOK()) {
        return {commitCloneStatus.code(),
                str::stream() << "commit clone failed due to " << commitCloneStatus.toString()};
    }

    // applyOps preparation for reflecting the uncommitted metadata on the config server

    // Preconditions
    BSONArrayBuilder preCond;
    {
        BSONObjBuilder b;
        b.append("ns", ChunkType::ConfigNS);
        b.append("q",
                 BSON("query" << BSON(ChunkType::ns(_args.getNss().ns())) << "orderby"
                              << BSON(ChunkType::DEPRECATED_lastmod() << -1)));
        {
            BSONObjBuilder bb(b.subobjStart("res"));

            // TODO: For backwards compatibility, we can't yet require an epoch here
            bb.appendTimestamp(ChunkType::DEPRECATED_lastmod(),
                               _committedMetadata->getCollVersion().toLong());
            bb.done();
        }

        preCond.append(b.obj());
    }

    // Update for the chunk which is being donated
    BSONArrayBuilder updates;
    {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);  // No upserting
        op.append("ns", ChunkType::ConfigNS);

        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), Chunk::genID(_args.getNss().ns(), _args.getMinKey()));
        _uncommittedMetadata->getCollVersion().addToBSON(n, ChunkType::DEPRECATED_lastmod());
        n.append(ChunkType::ns(), _args.getNss().ns());
        n.append(ChunkType::min(), _args.getMinKey());
        n.append(ChunkType::max(), _args.getMaxKey());
        n.append(ChunkType::shard(), _args.getToShardId());
        n.done();

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), Chunk::genID(_args.getNss().ns(), _args.getMinKey()));
        q.done();

        updates.append(op.obj());
    }

    // Update for the chunk being moved

    // Version at which the next highest lastmod will be set. If the chunk being moved is the last
    // in the shard, nextVersion is that chunk's lastmod otherwise the highest version is from the
    // chunk being bumped on the FROM-shard.
    ChunkVersion nextVersion = _uncommittedMetadata->getCollVersion();

    // If we have chunks left on the FROM shard, update the version of one of them as well. We can
    // figure that out by grabbing the metadata as it has been changed.
    if (_uncommittedMetadata->getNumChunks()) {
        ChunkType bumpChunk;
        invariant(
            _uncommittedMetadata->getNextChunk(_uncommittedMetadata->getMinKey(), &bumpChunk));

        BSONObj bumpMin = bumpChunk.getMin();
        BSONObj bumpMax = bumpChunk.getMax();
        nextVersion.incMinor();

        dassert(bumpMin.woCompare(_args.getMinKey()) != 0);

        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);
        op.append("ns", ChunkType::ConfigNS);

        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), Chunk::genID(_args.getNss().ns(), bumpMin));
        nextVersion.addToBSON(n, ChunkType::DEPRECATED_lastmod());
        n.append(ChunkType::ns(), _args.getNss().ns());
        n.append(ChunkType::min(), bumpMin);
        n.append(ChunkType::max(), bumpMax);
        n.append(ChunkType::shard(), _args.getFromShardId());
        n.done();

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), Chunk::genID(_args.getNss().ns(), bumpMin));
        q.done();

        updates.append(op.obj());

        log() << "moveChunk updating self version to: " << nextVersion << " through " << bumpMin
              << " -> " << bumpMax << " for collection '" << _args.getNss().ns() << "'";
    } else {
        log() << "moveChunk moved last chunk out for collection '" << _args.getNss().ns() << "'";
    }

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeCommitMigration);

    Status applyOpsStatus = grid.catalogManager(txn)->applyChunkOpsDeprecated(
        txn, updates.arr(), preCond.arr(), _args.getNss().ns(), nextVersion);

    // We cannot meaningfully recover from failure to reflect the uncommitted metadata on the config
    // servers, so crash the server.
    fassertStatusOK(34431, applyOpsStatus);

    // Now that applyOps succeeded, zero out the uncommitted metadata since it is now valid
    _committedMetadata = std::move(_uncommittedMetadata);

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeLeavingCriticalSection);

    _critSec->exitCriticalSection();

    grid.catalogManager(txn)
        ->logChange(txn,
                    "moveChunk.commit",
                    _args.getNss().ns(),
                    BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                               << _args.getFromShardId() << "to" << _args.getToShardId()));

    scopedGuard.Dismiss();
    _cleanup(txn);

    return Status::OK();
}

void MigrationSourceManager::cleanupOnError(OperationContext* txn) {
    if (_state == kDone) {
        return;
    }

    grid.catalogManager(txn)
        ->logChange(txn,
                    "moveChunk.error",
                    _args.getNss().ns(),
                    BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                               << _args.getFromShardId() << "to" << _args.getToShardId()));

    _cleanup(txn);
}

void MigrationSourceManager::_cleanup(OperationContext* txn) {
    invariant(_state != kDone);

    {
        // Unregister from the collection's sharding state
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, _args.getNss(), MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(txn, _args.getNss().ns());

        // The migration source manager is not visible anymore after it is unregistered from the
        // collection
        css->clearMigrationSourceManager(txn);

        // Restore the uncommitted metadata on the collection
        if (_state == kCriticalSection) {
            css->setMetadata(_committedMetadata);
            _uncommittedMetadata.reset();
            _critSec->exitCriticalSection();
            _critSec.reset();
        }
    }

    // Decrement the metadata op counter outside of the collection lock in order to hold it for as
    // short as possible.
    if (_state == kCriticalSection) {
        ShardingStateRecovery::endMetadataOp(txn);
    }

    if (_cloneDriver) {
        _cloneDriver->cancelClone(txn);
        _cloneDriver.reset();
    }

    _state = kDone;
}

MigrationSourceManager::CriticalSectionState::CriticalSectionState() = default;

bool MigrationSourceManager::CriticalSectionState::waitUntilOutOfCriticalSection(
    Microseconds waitTimeout) {
    const auto waitDeadline = stdx::chrono::system_clock::now() + waitTimeout;

    stdx::unique_lock<stdx::mutex> sl(_criticalSectionMutex);
    while (_inCriticalSection) {
        if (stdx::cv_status::timeout == _criticalSectionCV.wait_until(sl, waitDeadline)) {
            return false;
        }
    }

    return true;
}

void MigrationSourceManager::CriticalSectionState::exitCriticalSection() {
    stdx::unique_lock<stdx::mutex> sl(_criticalSectionMutex);
    _inCriticalSection = false;
    _criticalSectionCV.notify_all();
}

}  // namespace mongo

/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/s/migration_impl.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/logger/ramlog.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using str::stream;

namespace {

Tee* const migrateLog = RamLog::get("migrate");

BSONObj createRecvChunkCommitRequest(const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("_recvChunkCommit", 1);
    sessionId.append(&builder);
    return builder.obj();
}

MONGO_FP_DECLARE(failMigrationCommit);
MONGO_FP_DECLARE(hangBeforeCommitMigration);
MONGO_FP_DECLARE(hangBeforeLeavingCriticalSection);

}  // namespace

ChunkMoveOperationState::ChunkMoveOperationState(OperationContext* txn, NamespaceString ns)
    : _txn(txn), _nss(std::move(ns)) {}

ChunkMoveOperationState::~ChunkMoveOperationState() {
    if (!_isRunning) {
        return;
    }

    auto migrationSourceManager = ShardingState::get(_txn)->migrationSourceManager();
    invariant(migrationSourceManager);
    migrationSourceManager->done(_txn);
}

Status ChunkMoveOperationState::initialize(const BSONObj& cmdObj) {
    _fromShard = cmdObj["fromShard"].str();
    if (_fromShard.empty()) {
        return {ErrorCodes::InvalidOptions, "need to specify shard to move chunk from"};
    }

    _toShard = cmdObj["toShard"].str();
    if (_toShard.empty()) {
        return {ErrorCodes::InvalidOptions, "need to specify shard to move chunk to"};
    }

    _minKey = cmdObj["min"].Obj();
    if (_minKey.isEmpty()) {
        return {ErrorCodes::InvalidOptions, "need to specify a min"};
    }

    _maxKey = cmdObj["max"].Obj();
    if (_maxKey.isEmpty()) {
        return {ErrorCodes::InvalidOptions, "need to specify a max"};
    }

    {
        std::shared_ptr<Shard> fromShard = grid.shardRegistry()->getShard(_txn, _fromShard);
        if (!fromShard) {
            return {ErrorCodes::ShardNotFound,
                    stream() << "Source shard " << _fromShard
                             << " is missing. This indicates metadata corruption."};
        }

        _fromShardCS = fromShard->getConnString();
    }

    {
        std::shared_ptr<Shard> toShard = grid.shardRegistry()->getShard(_txn, _toShard);
        if (!toShard) {
            return {ErrorCodes::ShardNotFound,
                    stream() << "Destination shard " << _toShard
                             << " is missing. This indicates metadata corruption."};
        }

        _toShardCS = toShard->getConnString();
    }

    auto& oss = OperationShardingState::get(_txn);
    if (!oss.hasShardVersion()) {
        return Status{ErrorCodes::InvalidOptions, "moveChunk command is missing shard version"};
    }

    _collectionVersion = oss.getShardVersion(_nss);

    return Status::OK();
}

Status ChunkMoveOperationState::acquireMoveMetadata() {
    ShardingState* const shardingState = ShardingState::get(_txn);

    // Snapshot the metadata
    Status refreshStatus = shardingState->refreshMetadataNow(_txn, _nss.ns(), &_shardVersion);
    if (!refreshStatus.isOK()) {
        const string msg = stream() << "moveChunk cannot start migrate of chunk "
                                    << "[" << _minKey << "," << _maxKey << ")"
                                    << causedBy(refreshStatus.reason());
        warning() << msg;
        return Status(refreshStatus.code(), msg);
    }

    if (_shardVersion.majorVersion() == 0) {
        // It makes no sense to migrate if our version is zero and we have no chunks
        const string msg = stream() << "moveChunk cannot start migrate of chunk "
                                    << "[" << _minKey << "," << _maxKey << ")"
                                    << " with zero shard version";
        warning() << msg;
        return Status(ErrorCodes::IncompatibleShardingMetadata, msg);
    }

    if (_collectionVersion.epoch() != _shardVersion.epoch()) {
        const string msg = stream() << "moveChunk cannot move chunk "
                                    << "[" << _minKey << "," << _maxKey << "), "
                                    << "collection may have been dropped. "
                                    << "current epoch: " << _shardVersion.epoch()
                                    << ", cmd epoch: " << _collectionVersion.epoch();
        warning() << msg;
        throw SendStaleConfigException(_nss.toString(), msg, _collectionVersion, _shardVersion);
    }

    _collMetadata = shardingState->getCollectionMetadata(_nss.ns());

    // With nonzero shard version, we must have a coll version >= our shard version
    invariant(_collMetadata->getCollVersion() >= _shardVersion);

    // With nonzero shard version, we must have a shard key
    invariant(!_collMetadata->getKeyPattern().isEmpty());

    ChunkType origChunk;
    if (!_collMetadata->getNextChunk(_minKey, &origChunk) ||
        origChunk.getMin().woCompare(_minKey) || origChunk.getMax().woCompare(_maxKey)) {
        // Our boundaries are different from those passed in
        const string msg = stream() << "moveChunk cannot find chunk "
                                    << "[" << _minKey << "," << _maxKey << ")"
                                    << " to migrate, the chunk boundaries may be stale";
        warning() << msg;
        throw SendStaleConfigException(_nss.toString(), msg, _collectionVersion, _shardVersion);
    }

    return Status::OK();
}

Status ChunkMoveOperationState::commitMigration(const MigrationSessionId& sessionId) {
    log() << "About to enter migrate critical section";

    // We're under the collection distributed lock here, so no other migrate can change maxVersion
    // or CollectionMetadata state.
    ShardingState* const shardingState = ShardingState::get(_txn);

    Status startStatus = ShardingStateRecovery::startMetadataOp(_txn);
    if (!startStatus.isOK()) {
        warning() << "Failed to write sharding state recovery document" << causedBy(startStatus);
        return startStatus;
    }

    shardingState->migrationSourceManager()->setInCriticalSection(true);

    const ChunkVersion originalCollVersion = getCollMetadata()->getCollVersion();

    ChunkVersion myVersion = originalCollVersion;
    myVersion.incMajor();

    {
        ScopedTransaction transaction(_txn, MODE_IX);
        Lock::DBLock lk(_txn->lockState(), _nss.db(), MODE_IX);
        Lock::CollectionLock collLock(_txn->lockState(), _nss.ns(), MODE_X);

        invariant(myVersion > shardingState->getVersion(_nss.ns()));

        // Bump the metadata's version up and "forget" about the chunk being moved. This is
        // not the commit point, but in practice the state in this shard won't change until
        // the commit it done.
        shardingState->donateChunk(_txn, _nss.ns(), _minKey, _maxKey, myVersion);
    }

    log() << "moveChunk setting version to: " << myVersion << migrateLog;

    // We're under the collection lock here, too, so we can undo the chunk donation because
    // no other state change could be ongoing
    BSONObj res;
    Status recvChunkCommitStatus{ErrorCodes::InternalError, "status not set"};

    try {
        ScopedDbConnection connTo(_toShardCS, 35.0);
        connTo->runCommand("admin", createRecvChunkCommitRequest(sessionId), res);
        connTo.done();
        recvChunkCommitStatus = getStatusFromCommandResult(res);
    } catch (const DBException& e) {
        const string msg = stream() << "moveChunk could not contact to shard " << _toShard
                                    << " to commit transfer" << causedBy(e);
        warning() << msg;
        recvChunkCommitStatus = Status(e.toStatus().code(), msg);
    }

    if (MONGO_FAIL_POINT(failMigrationCommit) && recvChunkCommitStatus.isOK()) {
        recvChunkCommitStatus =
            Status(ErrorCodes::InternalError, "Failing _recvChunkCommit due to failpoint.");
    }

    if (!recvChunkCommitStatus.isOK()) {
        log() << "moveChunk migrate commit not accepted by TO-shard: " << res
              << " resetting shard version to: " << getShardVersion() << migrateLog;

        {
            ScopedTransaction transaction(_txn, MODE_IX);
            Lock::DBLock dbLock(_txn->lockState(), _nss.db(), MODE_IX);
            Lock::CollectionLock collLock(_txn->lockState(), _nss.ns(), MODE_X);

            log() << "moveChunk collection lock acquired to reset shard version from "
                     "failed migration";

            // Revert the chunk manager back to the state before "forgetting" about the chunk
            shardingState->undoDonateChunk(_txn, _nss.ns(), getCollMetadata());
        }

        log() << "Shard version successfully reset to clean up failed migration";

        return Status(recvChunkCommitStatus.code(),
                      stream() << "_recvChunkCommit failed: " << causedBy(recvChunkCommitStatus));
    }

    log() << "moveChunk migrate commit accepted by TO-shard: " << res << migrateLog;

    BSONArrayBuilder updates;

    {
        // Update for the chunk being moved
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);  // No upserting
        op.append("ns", ChunkType::ConfigNS);

        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), Chunk::genID(_nss.ns(), _minKey));
        myVersion.addToBSON(n, ChunkType::DEPRECATED_lastmod());
        n.append(ChunkType::ns(), _nss.ns());
        n.append(ChunkType::min(), _minKey);
        n.append(ChunkType::max(), _maxKey);
        n.append(ChunkType::shard(), _toShard);
        n.done();

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), Chunk::genID(_nss.ns(), _minKey));
        q.done();

        updates.append(op.obj());
    }

    // Version at which the next highest lastmod will be set. If the chunk being moved is the last
    // in the shard, nextVersion is that chunk's lastmod otherwise the highest version is from the
    // chunk being bumped on the FROM-shard.
    ChunkVersion nextVersion = myVersion;

    // If we have chunks left on the FROM shard, update the version of one of them as well. We can
    // figure that out by grabbing the metadata as it has been changed.
    const std::shared_ptr<CollectionMetadata> bumpedCollMetadata(
        shardingState->getCollectionMetadata(_nss.ns()));
    if (bumpedCollMetadata->getNumChunks() > 0) {
        // get another chunk on that shard
        ChunkType bumpChunk;
        invariant(bumpedCollMetadata->getNextChunk(bumpedCollMetadata->getMinKey(), &bumpChunk));

        BSONObj bumpMin = bumpChunk.getMin();
        BSONObj bumpMax = bumpChunk.getMax();

        dassert(bumpMin.woCompare(_minKey) != 0);

        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);
        op.append("ns", ChunkType::ConfigNS);

        nextVersion.incMinor();  // same as used on donateChunk

        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), Chunk::genID(_nss.ns(), bumpMin));
        nextVersion.addToBSON(n, ChunkType::DEPRECATED_lastmod());
        n.append(ChunkType::ns(), _nss.ns());
        n.append(ChunkType::min(), bumpMin);
        n.append(ChunkType::max(), bumpMax);
        n.append(ChunkType::shard(), _fromShard);
        n.done();

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), Chunk::genID(_nss.ns(), bumpMin));
        q.done();

        updates.append(op.obj());

        log() << "moveChunk updating self version to: " << nextVersion << " through " << bumpMin
              << " -> " << bumpMax << " for collection '" << _nss.ns() << "'" << migrateLog;
    } else {
        log() << "moveChunk moved last chunk out for collection '" << _nss.ns() << "'"
              << migrateLog;
    }

    BSONArrayBuilder preCond;
    {
        BSONObjBuilder b;
        b.append("ns", ChunkType::ConfigNS);
        b.append("q",
                 BSON("query" << BSON(ChunkType::ns(_nss.ns())) << "orderby"
                              << BSON(ChunkType::DEPRECATED_lastmod() << -1)));
        {
            BSONObjBuilder bb(b.subobjStart("res"));

            // TODO: For backwards compatibility, we can't yet require an epoch here
            bb.appendTimestamp(ChunkType::DEPRECATED_lastmod(), originalCollVersion.toLong());
            bb.done();
        }

        preCond.append(b.obj());
    }

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeCommitMigration);

    fassertStatusOK(34431,
                    grid.catalogManager(_txn)->applyChunkOpsDeprecated(
                        _txn, updates.arr(), preCond.arr(), _nss.ns(), nextVersion));

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeLeavingCriticalSection);

    shardingState->migrationSourceManager()->setInCriticalSection(false);
    ShardingStateRecovery::endMetadataOp(_txn);

    // Migration is done, just log some diagnostics information
    BSONObj chunkInfo =
        BSON("min" << _minKey << "max" << _maxKey << "from" << _fromShard << "to" << _toShard);

    BSONObjBuilder commitInfo;
    commitInfo.appendElements(chunkInfo);
    if (res["counts"].type() == Object) {
        commitInfo.appendElements(res["counts"].Obj());
    }

    grid.catalogManager(_txn)->logChange(_txn, "moveChunk.commit", _nss.ns(), commitInfo.obj());

    shardingState->migrationSourceManager()->done(_txn);
    _isRunning = false;

    return Status::OK();
}

ChunkVersion ChunkMoveOperationState::getShardVersion() const {
    return _shardVersion;
}

std::shared_ptr<CollectionMetadata> ChunkMoveOperationState::getCollMetadata() const {
    return _collMetadata;
}

Status ChunkMoveOperationState::start(const MigrationSessionId& sessionId,
                                      const BSONObj& shardKeyPattern) {
    auto migrationSourceManager = ShardingState::get(_txn)->migrationSourceManager();
    if (!migrationSourceManager->start(
            _txn, sessionId, _nss.ns(), _minKey, _maxKey, shardKeyPattern)) {
        return {ErrorCodes::ConflictingOperationInProgress,
                "Not starting chunk migration because another migration is already in progress "
                "from this shard"};
    }

    _isRunning = true;

    return Status::OK();
}

}  // namespace mongo

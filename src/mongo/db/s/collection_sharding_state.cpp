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

#include "mongo/db/s/collection_sharding_state.h"

#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"

namespace mongo {

namespace {

using std::string;

/**
 * Used to perform shard identity initialization once it is certain that the document is committed.
 */
class ShardIdentityLogOpHandler final : public RecoveryUnit::Change {
public:
    ShardIdentityLogOpHandler(OperationContext* txn, ShardIdentityType shardIdentity)
        : _txn(txn), _shardIdentity(std::move(shardIdentity)) {}

    void commit() override {
        fassertNoTrace(40071,
                       ShardingState::get(_txn)->initializeFromShardIdentity(
                           _txn, _shardIdentity, Date_t::max()));
    }

    void rollback() override {}

private:
    OperationContext* _txn;
    const ShardIdentityType _shardIdentity;
};

/**
 * Used by the config server for backwards compatibility with 3.2 mongos to upsert a shardIdentity
 * document (and thereby perform shard aware initialization) on a newly added shard.
 */
class LegacyAddShardLogOpHandler final : public RecoveryUnit::Change {
public:
    LegacyAddShardLogOpHandler(OperationContext* txn, ShardType shardType)
        : _txn(txn), _shardType(std::move(shardType)) {}

    void commit() override {
        // Only the primary should complete the addShard process by upserting the shardIdentity on
        // the new shard.
        if (repl::getGlobalReplicationCoordinator()->getMemberState().primary()) {
            uassertStatusOK(
                Grid::get(_txn)->catalogManager()->upsertShardIdentityOnShard(_txn, _shardType));
        }
    }

    void rollback() override {}

private:
    OperationContext* _txn;
    const ShardType _shardType;
};

}  // unnamed namespace

CollectionShardingState::CollectionShardingState(
    NamespaceString nss, std::unique_ptr<CollectionMetadata> initialMetadata)
    : _nss(std::move(nss)), _metadataManager{} {
    _metadataManager.setActiveMetadata(std::move(initialMetadata));
}

CollectionShardingState::~CollectionShardingState() {
    invariant(!_sourceMgr);
}

CollectionShardingState* CollectionShardingState::get(OperationContext* txn,
                                                      const NamespaceString& nss) {
    return CollectionShardingState::get(txn, nss.ns());
}

CollectionShardingState* CollectionShardingState::get(OperationContext* txn,
                                                      const std::string& ns) {
    // Collection lock must be held to have a reference to the collection's sharding state
    dassert(txn->lockState()->isCollectionLockedForMode(ns, MODE_IS));

    ShardingState* const shardingState = ShardingState::get(txn);
    return shardingState->getNS(ns);
}

ScopedCollectionMetadata CollectionShardingState::getMetadata() {
    return _metadataManager.getActiveMetadata();
}

void CollectionShardingState::setMetadata(std::unique_ptr<CollectionMetadata> newMetadata) {
    if (newMetadata) {
        invariant(!newMetadata->getCollVersion().isWriteCompatibleWith(ChunkVersion::UNSHARDED()));
        invariant(!newMetadata->getShardVersion().isWriteCompatibleWith(ChunkVersion::UNSHARDED()));
    }
    _metadataManager.setActiveMetadata(std::move(newMetadata));
}

MigrationSourceManager* CollectionShardingState::getMigrationSourceManager() {
    return _sourceMgr;
}

void CollectionShardingState::setMigrationSourceManager(OperationContext* txn,
                                                        MigrationSourceManager* sourceMgr) {
    invariant(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    invariant(sourceMgr);
    invariant(!_sourceMgr);

    _sourceMgr = sourceMgr;
}

void CollectionShardingState::clearMigrationSourceManager(OperationContext* txn) {
    invariant(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    invariant(_sourceMgr);

    _sourceMgr = nullptr;
}

void CollectionShardingState::checkShardVersionOrThrow(OperationContext* txn) {
    string errmsg;
    ChunkVersion received;
    ChunkVersion wanted;
    if (!_checkShardVersionOk(txn, &errmsg, &received, &wanted)) {
        throw SendStaleConfigException(
            _nss.ns(),
            str::stream() << "[" << _nss.ns() << "] shard version not ok: " << errmsg,
            received,
            wanted);
    }
}

bool CollectionShardingState::isDocumentInMigratingChunk(OperationContext* txn,
                                                         const BSONObj& doc) {
    dassert(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    if (_sourceMgr) {
        return _sourceMgr->getCloner()->isDocumentInMigratingChunk(txn, doc);
    }

    return false;
}

void CollectionShardingState::onInsertOp(OperationContext* txn, const BSONObj& insertedDoc) {
    dassert(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
        _nss == NamespaceString::kConfigCollectionNamespace) {
        if (auto idElem = insertedDoc["_id"]) {
            if (idElem.str() == ShardIdentityType::IdName) {
                auto shardIdentityDoc = uassertStatusOK(ShardIdentityType::fromBSON(insertedDoc));
                uassertStatusOK(shardIdentityDoc.validate());
                txn->recoveryUnit()->registerChange(
                    new ShardIdentityLogOpHandler(txn, std::move(shardIdentityDoc)));
            }
        }
    }

    // For backwards compatibility with 3.2 mongos, perform share aware initialization on a newly
    // added shard on inserts to config.shards missing the "state" field. (On addShard, a 3.2
    // mongos performs the insert into config.shards without a "state" field.)
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
        _nss == ShardType::ConfigNS) {
        if (insertedDoc[ShardType::state.name()].eoo()) {
            const auto shardType = uassertStatusOK(ShardType::fromBSON(insertedDoc));
            txn->recoveryUnit()->registerChange(
                new LegacyAddShardLogOpHandler(txn, std::move(shardType)));
        }
    }

    checkShardVersionOrThrow(txn);

    if (_sourceMgr) {
        _sourceMgr->getCloner()->onInsertOp(txn, insertedDoc);
    }
}

void CollectionShardingState::onUpdateOp(OperationContext* txn, const BSONObj& updatedDoc) {
    dassert(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    checkShardVersionOrThrow(txn);

    if (_sourceMgr) {
        _sourceMgr->getCloner()->onUpdateOp(txn, updatedDoc);
    }
}

void CollectionShardingState::onDeleteOp(OperationContext* txn, const BSONObj& deletedDocId) {
    dassert(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    if (txn->writesAreReplicated() && serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
        _nss == NamespaceString::kConfigCollectionNamespace) {
        if (auto idElem = deletedDocId["_id"]) {
            uassert(40070,
                    "cannot delete shardIdentity document while in --shardsvr mode",
                    idElem.str() != ShardIdentityType::IdName);
        }
    }

    checkShardVersionOrThrow(txn);

    if (_sourceMgr) {
        _sourceMgr->getCloner()->onDeleteOp(txn, deletedDocId);
    }
}

bool CollectionShardingState::_checkShardVersionOk(OperationContext* txn,
                                                   string* errmsg,
                                                   ChunkVersion* expectedShardVersion,
                                                   ChunkVersion* actualShardVersion) {
    Client* client = txn->getClient();

    // Operations using the DBDirectClient are unversioned.
    if (client->isInDirectClient()) {
        return true;
    }

    if (!repl::ReplicationCoordinator::get(txn)->canAcceptWritesForDatabase(_nss.db())) {
        // Right now connections to secondaries aren't versioned at all.
        return true;
    }

    const auto& oss = OperationShardingState::get(txn);

    // If there is a version attached to the OperationContext, use it as the received version.
    // Otherwise, get the received version from the ShardedConnectionInfo.
    if (oss.hasShardVersion()) {
        *expectedShardVersion = oss.getShardVersion(_nss);
    } else {
        ShardedConnectionInfo* info = ShardedConnectionInfo::get(client, false);
        if (!info) {
            // There is no shard version information on either 'txn' or 'client'. This means that
            // the operation represented by 'txn' is unversioned, and the shard version is always OK
            // for unversioned operations.
            return true;
        }

        *expectedShardVersion = info->getVersion(_nss.ns());
    }

    if (ChunkVersion::isIgnoredVersion(*expectedShardVersion)) {
        return true;
    }

    // Set this for error messaging purposes before potentially returning false.
    auto metadata = getMetadata();
    *actualShardVersion = metadata ? metadata->getShardVersion() : ChunkVersion::UNSHARDED();

    if (_sourceMgr && _sourceMgr->getMigrationCriticalSectionSignal()) {
        *errmsg = str::stream() << "migration commit in progress for " << _nss.ns();

        // Set migration critical section on operation sharding state: operation will wait for the
        // migration to finish before returning failure and retrying.
        OperationShardingState::get(txn).setMigrationCriticalSectionSignal(
            _sourceMgr->getMigrationCriticalSectionSignal());
        return false;
    }

    if (expectedShardVersion->isWriteCompatibleWith(*actualShardVersion)) {
        return true;
    }

    //
    // Figure out exactly why not compatible, send appropriate error message
    // The versions themselves are returned in the error, so not needed in messages here
    //

    // Check epoch first, to send more meaningful message, since other parameters probably won't
    // match either.
    if (actualShardVersion->epoch() != expectedShardVersion->epoch()) {
        *errmsg = str::stream() << "version epoch mismatch detected for " << _nss.ns() << ", "
                                << "the collection may have been dropped and recreated";
        return false;
    }

    if (!actualShardVersion->isSet() && expectedShardVersion->isSet()) {
        *errmsg = str::stream() << "this shard no longer contains chunks for " << _nss.ns() << ", "
                                << "the collection may have been dropped";
        return false;
    }

    if (actualShardVersion->isSet() && !expectedShardVersion->isSet()) {
        *errmsg = str::stream() << "this shard contains versioned chunks for " << _nss.ns() << ", "
                                << "but no version set in request";
        return false;
    }

    if (actualShardVersion->majorVersion() != expectedShardVersion->majorVersion()) {
        // Could be > or < - wanted is > if this is the source of a migration, wanted < if this is
        // the target of a migration
        *errmsg = str::stream() << "version mismatch detected for " << _nss.ns();
        return false;
    }

    // Those are all the reasons the versions can mismatch
    MONGO_UNREACHABLE;
}

}  // namespace mongo

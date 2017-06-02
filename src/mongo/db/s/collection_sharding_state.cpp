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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

using std::string;

/**
 * Used to perform shard identity initialization once it is certain that the document is committed.
 */
class ShardIdentityLogOpHandler final : public RecoveryUnit::Change {
public:
    ShardIdentityLogOpHandler(OperationContext* opCtx, ShardIdentityType shardIdentity)
        : _opCtx(opCtx), _shardIdentity(std::move(shardIdentity)) {}

    void commit() override {
        fassertNoTrace(
            40071, ShardingState::get(_opCtx)->initializeFromShardIdentity(_opCtx, _shardIdentity));
    }

    void rollback() override {}

private:
    OperationContext* _opCtx;
    const ShardIdentityType _shardIdentity;
};

/**
 * Checks via the ReplicationCoordinator whether this server is a primary/standalone that can do
 * writes. This function may return false if the server is primary but in drain mode.
 *
 * Note: expects the global lock to be held so that a meaningful answer is returned -- replica set
 * state cannot change under a lock.
 */
bool isPrimary(OperationContext* opCtx, const NamespaceString& nss) {
    // If the server can execute writes, then it is either a primary or standalone.
    return repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);
}

}  // unnamed namespace

CollectionShardingState::CollectionShardingState(ServiceContext* sc, NamespaceString nss)
    : _nss(std::move(nss)),
      _metadataManager(std::make_shared<MetadataManager>(
          sc, _nss, ShardingState::get(sc)->getRangeDeleterTaskExecutor())) {}

CollectionShardingState::~CollectionShardingState() {
    invariant(!_sourceMgr);
}

CollectionShardingState* CollectionShardingState::get(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    return CollectionShardingState::get(opCtx, nss.ns());
}

CollectionShardingState* CollectionShardingState::get(OperationContext* opCtx,
                                                      const std::string& ns) {
    // Collection lock must be held to have a reference to the collection's sharding state
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns, MODE_IS));

    ShardingState* const shardingState = ShardingState::get(opCtx);
    return shardingState->getNS(ns, opCtx);
}

ScopedCollectionMetadata CollectionShardingState::getMetadata() {
    return _metadataManager->getActiveMetadata(_metadataManager);
}

void CollectionShardingState::refreshMetadata(OperationContext* opCtx,
                                              std::unique_ptr<CollectionMetadata> newMetadata) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));

    _metadataManager->refreshActiveMetadata(std::move(newMetadata));
}

void CollectionShardingState::markNotShardedAtStepdown() {
    _metadataManager->refreshActiveMetadata(nullptr);
}

auto CollectionShardingState::beginReceive(ChunkRange const& range) -> CleanupNotification {
    return _metadataManager->beginReceive(range);
}

void CollectionShardingState::forgetReceive(const ChunkRange& range) {
    _metadataManager->forgetReceive(range);
}

auto CollectionShardingState::cleanUpRange(ChunkRange const& range) -> CleanupNotification {
    return _metadataManager->cleanUpRange(range);
}

auto CollectionShardingState::overlappingMetadata(ChunkRange const& range) const
    -> std::vector<ScopedCollectionMetadata> {
    return _metadataManager->overlappingMetadata(_metadataManager, range);
}


MigrationSourceManager* CollectionShardingState::getMigrationSourceManager() {
    return _sourceMgr;
}

void CollectionShardingState::setMigrationSourceManager(OperationContext* opCtx,
                                                        MigrationSourceManager* sourceMgr) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    invariant(sourceMgr);
    invariant(!_sourceMgr);

    _sourceMgr = sourceMgr;
}

void CollectionShardingState::clearMigrationSourceManager(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    invariant(_sourceMgr);

    _sourceMgr = nullptr;
}

void CollectionShardingState::checkShardVersionOrThrow(OperationContext* opCtx) {
    string errmsg;
    ChunkVersion received;
    ChunkVersion wanted;
    if (!_checkShardVersionOk(opCtx, &errmsg, &received, &wanted)) {
        throw SendStaleConfigException(
            _nss.ns(),
            str::stream() << "[" << _nss.ns() << "] shard version not ok: " << errmsg,
            received,
            wanted);
    }
}

bool CollectionShardingState::collectionIsSharded() {
    auto metadata = getMetadata().getMetadata();
    if (metadata && (metadata->getCollVersion().isStrictlyEqualTo(ChunkVersion::UNSHARDED()))) {
        return false;
    }

    // If 'metadata' is null, then the shard doesn't know if this collection is sharded or not. In
    // this scenario we will assume this collection is sharded. We will know sharding state
    // definitively once SERVER-24960 has been fixed.
    return true;
}

// Call with collection unlocked.  Note that the CollectionShardingState object involved might not
// exist anymore at the time of the call, or indeed anytime outside the AutoGetCollection block, so
// anything that might alias something in it must be copied first.

/* static */
Status CollectionShardingState::waitForClean(OperationContext* opCtx,
                                             NamespaceString nss,
                                             OID const& epoch,
                                             ChunkRange orphanRange) {
    do {
        auto stillScheduled = boost::optional<CleanupNotification>();
        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            // First, see if collection was dropped.
            auto css = CollectionShardingState::get(opCtx, nss);
            {
                auto metadata = css->_metadataManager->getActiveMetadata(css->_metadataManager);
                if (!metadata || metadata->getCollVersion().epoch() != epoch) {
                    return {ErrorCodes::StaleShardVersion, "Collection being migrated was dropped"};
                }
            }  // drop metadata
            stillScheduled = css->trackOrphanedDataCleanup(orphanRange);
            if (!stillScheduled) {
                log() << "Finished deleting " << nss.ns() << " range "
                      << redact(orphanRange.toString());
                return Status::OK();
            }
        }  // drop collection lock

        log() << "Waiting for deletion of " << nss.ns() << " range " << orphanRange;
        Status result = stillScheduled->waitStatus(opCtx);
        if (!result.isOK()) {
            return Status{result.code(),
                          str::stream() << "Failed to delete orphaned " << nss.ns() << " range "
                                        << orphanRange.toString()
                                        << ": "
                                        << result.reason()};
        }
    } while (true);
    MONGO_UNREACHABLE;
}

auto CollectionShardingState::trackOrphanedDataCleanup(ChunkRange const& range)
    -> boost::optional<CleanupNotification> {
    return _metadataManager->trackOrphanedDataCleanup(range);
}

boost::optional<KeyRange> CollectionShardingState::getNextOrphanRange(BSONObj const& from) {
    return _metadataManager->getNextOrphanRange(from);
}

bool CollectionShardingState::isDocumentInMigratingChunk(OperationContext* opCtx,
                                                         const BSONObj& doc) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    if (_sourceMgr) {
        return _sourceMgr->getCloner()->isDocumentInMigratingChunk(opCtx, doc);
    }

    return false;
}

void CollectionShardingState::onInsertOp(OperationContext* opCtx, const BSONObj& insertedDoc) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
        _nss == NamespaceString::kServerConfigurationNamespace) {
        if (auto idElem = insertedDoc["_id"]) {
            if (idElem.str() == ShardIdentityType::IdName) {
                auto shardIdentityDoc = uassertStatusOK(ShardIdentityType::fromBSON(insertedDoc));
                uassertStatusOK(shardIdentityDoc.validate());
                opCtx->recoveryUnit()->registerChange(
                    new ShardIdentityLogOpHandler(opCtx, std::move(shardIdentityDoc)));
            }
        }
    }

    checkShardVersionOrThrow(opCtx);

    if (_sourceMgr) {
        _sourceMgr->getCloner()->onInsertOp(opCtx, insertedDoc);
    }
}

void CollectionShardingState::onUpdateOp(OperationContext* opCtx,
                                         const BSONObj& query,
                                         const BSONObj& update,
                                         const BSONObj& updatedDoc) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    // TODO: should this be before or after my new code????
    checkShardVersionOrThrow(opCtx);

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
        _nss == NamespaceString::kShardConfigCollectionsCollectionName) {
        _onConfigRefreshCompleteInvalidateCachedMetadata(opCtx, query, update);
    }

    if (_sourceMgr) {
        _sourceMgr->getCloner()->onUpdateOp(opCtx, updatedDoc);
    }
}

void CollectionShardingState::onDeleteOp(OperationContext* opCtx,
                                         const CollectionShardingState::DeleteState& deleteState) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        if (_nss == NamespaceString::kShardConfigCollectionsCollectionName) {
            _onConfigDeleteInvalidateCachedMetadata(opCtx, deleteState.idDoc);
        }

        if (_nss == NamespaceString::kServerConfigurationNamespace) {
            if (auto idElem = deleteState.idDoc["_id"]) {
                auto idStr = idElem.str();
                if (idStr == ShardIdentityType::IdName) {
                    if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
                        uasserted(40070,
                                  "cannot delete shardIdentity document while in --shardsvr mode");
                    } else {
                        warning() << "Shard identity document rolled back.  Will shut down after "
                                     "finishing rollback.";
                        ShardIdentityRollbackNotifier::get(opCtx)->recordThatRollbackHappened();
                    }
                }
            }
        }
    }

    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        if (_nss == VersionType::ConfigNS) {
            if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
                uasserted(40302, "cannot delete config.version document while in --configsvr mode");
            } else {
                // Throw out any cached information related to the cluster ID.
                Grid::get(opCtx)
                    ->catalogManager()
                    ->discardCachedConfigDatabaseInitializationState();
                ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
            }
        }
    }

    checkShardVersionOrThrow(opCtx);

    if (_sourceMgr && deleteState.isMigrating) {
        _sourceMgr->getCloner()->onDeleteOp(opCtx, deleteState.idDoc);
    }
}

void CollectionShardingState::onDropCollection(OperationContext* opCtx,
                                               const NamespaceString& collectionName) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
        _nss == NamespaceString::kServerConfigurationNamespace) {
        // Dropping system collections is not allowed for end users.
        invariant(!opCtx->writesAreReplicated());
        invariant(repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback());

        // Can't confirm whether there was a ShardIdentity document or not yet, so assume there was
        // one and shut down the process to clear the in-memory sharding state.
        warning() << "admin.system.version collection rolled back.  Will shut down after "
                     "finishing rollback";
        ShardIdentityRollbackNotifier::get(opCtx)->recordThatRollbackHappened();
    }

    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        if (_nss == VersionType::ConfigNS) {
            if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
                uasserted(40303, "cannot drop config.version document while in --configsvr mode");
            } else {
                // Throw out any cached information related to the cluster ID.
                Grid::get(opCtx)
                    ->catalogManager()
                    ->discardCachedConfigDatabaseInitializationState();
                ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
            }
        }
    }
}

void CollectionShardingState::_onConfigRefreshCompleteInvalidateCachedMetadata(
    OperationContext* opCtx, const BSONObj& query, const BSONObj& update) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    // If not primary, check whether a chunk metadata refresh just finished and invalidate the
    // catalog cache's in-memory chunk metadata cache if true.
    if (!isPrimary(opCtx, _nss)) {
        // Extract which collection entry is being updated
        std::string refreshCollection;
        fassertStatusOK(
            40477,
            bsonExtractStringField(query, ShardCollectionType::uuid.name(), &refreshCollection));

        // Parse the '$set' update, which will contain the 'refreshSequenceNumber' if it is present.
        BSONElement updateElement;
        fassertStatusOK(40478,
                        bsonExtractTypedField(update, StringData("$set"), Object, &updateElement));
        BSONObj setField = updateElement.Obj();

        // The refreshSequenceNumber is only updated when a chunk metadata refresh completes.
        if (setField.hasField(ShardCollectionType::refreshSequenceNumber.name())) {
            Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(refreshCollection);
        }
    }
}

void CollectionShardingState::_onConfigDeleteInvalidateCachedMetadata(OperationContext* opCtx,
                                                                      const BSONObj& query) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    // If not primary, invalidate the catalog cache's in-memory chunk metadata cache for the
    // collection specified in 'query'. The collection metadata has been dropped, so the cached
    // metadata must be invalidated.
    if (!isPrimary(opCtx, _nss)) {
        // Extract which collection entry is being deleted from the _id field.
        std::string deletedCollection;
        fassertStatusOK(
            40479,
            bsonExtractStringField(query, ShardCollectionType::uuid.name(), &deletedCollection));

        Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(deletedCollection);
    }
}

bool CollectionShardingState::_checkShardVersionOk(OperationContext* opCtx,
                                                   string* errmsg,
                                                   ChunkVersion* expectedShardVersion,
                                                   ChunkVersion* actualShardVersion) {
    Client* client = opCtx->getClient();

    if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx, _nss.db())) {
        // Right now connections to secondaries aren't versioned at all.
        return true;
    }

    auto& oss = OperationShardingState::get(opCtx);

    // If there is a version attached to the OperationContext, use it as the received version.
    // Otherwise, get the received version from the ShardedConnectionInfo.
    if (oss.hasShardVersion()) {
        *expectedShardVersion = oss.getShardVersion(_nss);
    } else {
        ShardedConnectionInfo* info = ShardedConnectionInfo::get(client, false);
        if (!info) {
            // There is no shard version information on either 'opCtx' or 'client'. This means that
            // the operation represented by 'opCtx' is unversioned, and the shard version is always
            // OK for unversioned operations.
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

    if (_sourceMgr) {
        const bool isReader = !opCtx->lockState()->isWriteLocked();

        auto criticalSectionSignal = _sourceMgr->getMigrationCriticalSectionSignal(isReader);
        if (criticalSectionSignal) {
            *errmsg = str::stream() << "migration commit in progress for " << _nss.ns();

            // Set migration critical section on operation sharding state: operation will wait for
            // the migration to finish before returning failure and retrying.
            oss.setMigrationCriticalSectionSignal(criticalSectionSignal);
            return false;
        }
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

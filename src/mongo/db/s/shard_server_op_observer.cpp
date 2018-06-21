/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_server_op_observer.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/catalog/type_shard_database.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto getDeleteState = OperationContext::declareDecoration<ShardObserverDeleteState>();

bool isStandaloneOrPrimary(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    return !isReplSet || (repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                          repl::MemberState::RS_PRIMARY);
}

/**
 * Used to notify the catalog cache loader of a new collection version and invalidate the in-memory
 * routing table cache once the oplog updates are committed and become visible.
 */
class CollectionVersionLogOpHandler final : public RecoveryUnit::Change {
public:
    CollectionVersionLogOpHandler(OperationContext* opCtx, const NamespaceString& nss)
        : _opCtx(opCtx), _nss(nss) {}

    void commit(boost::optional<Timestamp>) override {
        invariant(_opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

        CatalogCacheLoader::get(_opCtx).notifyOfCollectionVersionUpdate(_nss);

        // This is a hack to get around CollectionShardingState::refreshMetadata() requiring the X
        // lock: markNotShardedAtStepdown() doesn't have a lock check. Temporary measure until
        // SERVER-31595 removes the X lock requirement.
        CollectionShardingState::get(_opCtx, _nss)->markNotShardedAtStepdown();
    }

    void rollback() override {}

private:
    OperationContext* _opCtx;
    const NamespaceString _nss;
};

/**
 * Used to perform shard identity initialization once it is certain that the document is committed.
 */
class ShardIdentityLogOpHandler final : public RecoveryUnit::Change {
public:
    ShardIdentityLogOpHandler(OperationContext* opCtx, ShardIdentityType shardIdentity)
        : _opCtx(opCtx), _shardIdentity(std::move(shardIdentity)) {}

    void commit(boost::optional<Timestamp>) override {
        fassertNoTrace(
            40071, ShardingState::get(_opCtx)->initializeFromShardIdentity(_opCtx, _shardIdentity));
    }

    void rollback() override {}

private:
    OperationContext* _opCtx;
    const ShardIdentityType _shardIdentity;
};

/**
 * Invalidates the in-memory routing table cache when a collection is dropped, so the next caller
 * with routing information will provoke a routing table refresh and see the drop.
 *
 * The query parameter must contain an _id field that identifies which collections entry is being
 * updated.
 *
 * This only runs on secondaries.
 * The global exclusive lock is expected to be held by the caller.
 */
void onConfigDeleteInvalidateCachedCollectionMetadataAndNotify(OperationContext* opCtx,
                                                               const BSONObj& query) {
    // Notification of routing table changes are only needed on secondaries
    if (isStandaloneOrPrimary(opCtx)) {
        return;
    }

    // Extract which collection entry is being deleted from the _id field.
    std::string deletedCollection;
    fassert(40479,
            bsonExtractStringField(query, ShardCollectionType::ns.name(), &deletedCollection));
    const NamespaceString deletedNss(deletedCollection);

    // Need the WUOW to retain the lock for CollectionVersionLogOpHandler::commit().
    AutoGetCollection autoColl(opCtx, deletedNss, MODE_IX);

    opCtx->recoveryUnit()->registerChange(new CollectionVersionLogOpHandler(opCtx, deletedNss));
}

/**
 * Returns true if the total number of bytes on the specified chunk nears the max size of a shard.
 */
bool shouldSplitChunk(OperationContext* opCtx,
                      const ShardKeyPattern& shardKeyPattern,
                      const Chunk& chunk) {
    const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    invariant(balancerConfig);

    const auto& keyPattern = shardKeyPattern.getKeyPattern();
    const bool minIsInf = (0 == keyPattern.globalMin().woCompare(chunk.getMin()));
    const bool maxIsInf = (0 == keyPattern.globalMax().woCompare(chunk.getMax()));

    return chunk.shouldSplit(balancerConfig->getMaxChunkSizeBytes(), minIsInf, maxIsInf);
}

/**
 * If the collection is sharded, finds the chunk that contains the specified document and increments
 * the size tracked for that chunk by the specified amount of data written, in bytes. Returns the
 * number of total bytes on that chunk after the data is written.
 */
void incrementChunkOnInsertOrUpdate(OperationContext* opCtx,
                                    const ChunkManager& chunkManager,
                                    const BSONObj& document,
                                    long dataWritten) {
    const auto& shardKeyPattern = chunkManager.getShardKeyPattern();

    // Each inserted/updated document should contain the shard key. The only instance in which a
    // document could not contain a shard key is if the insert/update is performed through mongod
    // explicitly, as opposed to first routed through mongos.
    BSONObj shardKey = shardKeyPattern.extractShardKeyFromDoc(document);
    if (shardKey.woCompare(BSONObj()) == 0) {
        warning() << "inserting document " << document.toString() << " without shard key pattern "
                  << shardKeyPattern << " into a sharded collection";
        return;
    }

    // Use the shard key to locate the chunk into which the document was updated, and increment the
    // number of bytes tracked for the chunk.
    //
    // Note that we can assume the simple collation, because shard keys do not support non-simple
    // collations.
    auto chunk = chunkManager.findIntersectingChunkWithSimpleCollation(shardKey);
    chunk.addBytesWritten(dataWritten);

    // If the chunk becomes too large, then we call the ChunkSplitter to schedule a split. Then, we
    // reset the tracking for that chunk to 0.
    if (shouldSplitChunk(opCtx, shardKeyPattern, chunk)) {
        // TODO: call ChunkSplitter here
        chunk.clearBytesWritten();
    }
}

}  // namespace

ShardServerOpObserver::ShardServerOpObserver() = default;

ShardServerOpObserver::~ShardServerOpObserver() = default;

void ShardServerOpObserver::onInserts(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      OptionalCollectionUUID uuid,
                                      std::vector<InsertStatement>::const_iterator begin,
                                      std::vector<InsertStatement>::const_iterator end,
                                      bool fromMigrate) {
    auto const css = CollectionShardingState::get(opCtx, nss);
    const auto metadata = css->getMetadata(opCtx);

    for (auto it = begin; it != end; ++it) {
        const auto& insertedDoc = it->doc;

        if (nss == NamespaceString::kServerConfigurationNamespace) {
            if (auto idElem = insertedDoc["_id"]) {
                if (idElem.str() == ShardIdentityType::IdName) {
                    auto shardIdentityDoc =
                        uassertStatusOK(ShardIdentityType::fromShardIdentityDocument(insertedDoc));
                    uassertStatusOK(shardIdentityDoc.validate());
                    opCtx->recoveryUnit()->registerChange(
                        new ShardIdentityLogOpHandler(opCtx, std::move(shardIdentityDoc)));
                }
            }
        }

        if (metadata) {
            incrementChunkOnInsertOrUpdate(
                opCtx, *metadata->getChunkManager(), insertedDoc, insertedDoc.objsize());
        }
    }
}

void ShardServerOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    auto const css = CollectionShardingState::get(opCtx, args.nss);
    const auto metadata = css->getMetadata(opCtx);

    if (args.nss == NamespaceString::kShardConfigCollectionsNamespace) {
        // Notification of routing table changes are only needed on secondaries
        if (isStandaloneOrPrimary(opCtx)) {
            return;
        }

        // This logic runs on updates to the shard's persisted cache of the config server's
        // config.collections collection.
        //
        // If an update occurs to the 'lastRefreshedCollectionVersion' field it notifies the catalog
        // cache loader of a new collection version and clears the routing table so the next caller
        // with routing information will provoke a routing table refresh.
        //
        // When 'lastRefreshedCollectionVersion' is in 'update', it means that a chunk metadata
        // refresh has finished being applied to the collection's locally persisted metadata store.
        //
        // If an update occurs to the 'enterCriticalSectionSignal' field, simply clear the routing
        // table immediately. This will provoke the next secondary caller to refresh through the
        // primary, blocking behind the critical section.

        // Extract which user collection was updated
        const auto updatedNss([&] {
            std::string coll;
            fassert(40477,
                    bsonExtractStringField(args.criteria, ShardCollectionType::ns.name(), &coll));
            return NamespaceString(coll);
        }());

        // Parse the '$set' update
        BSONElement setElement;
        Status setStatus =
            bsonExtractTypedField(args.update, StringData("$set"), Object, &setElement);
        if (setStatus.isOK()) {
            BSONObj setField = setElement.Obj();

            // Need the WUOW to retain the lock for CollectionVersionLogOpHandler::commit()
            AutoGetCollection autoColl(opCtx, updatedNss, MODE_IX);

            if (setField.hasField(ShardCollectionType::refreshing.name()) &&
                !setField.getBoolField("refreshing")) {
                opCtx->recoveryUnit()->registerChange(
                    new CollectionVersionLogOpHandler(opCtx, updatedNss));
            }

            if (setField.hasField(ShardCollectionType::enterCriticalSectionCounter.name())) {
                // This is a hack to get around CollectionShardingState::refreshMetadata() requiring
                // the X lock: markNotShardedAtStepdown() doesn't have a lock check. Temporary
                // measure until SERVER-31595 removes the X lock requirement.
                CollectionShardingState::get(opCtx, updatedNss)->markNotShardedAtStepdown();
            }
        }
    }

    if (args.nss == NamespaceString::kShardConfigDatabasesNamespace) {
        // Notification of routing table changes are only needed on secondaries
        if (isStandaloneOrPrimary(opCtx)) {
            return;
        }

        // This logic runs on updates to the shard's persisted cache of the config server's
        // config.databases collection.
        //
        // If an update occurs to the 'enterCriticalSectionSignal' field, clear the routing
        // table immediately. This will provoke the next secondary caller to refresh through the
        // primary, blocking behind the critical section.

        // Extract which database was updated
        std::string db;
        fassert(40478, bsonExtractStringField(args.criteria, ShardDatabaseType::name.name(), &db));

        // Parse the '$set' update
        BSONElement setElement;
        Status setStatus =
            bsonExtractTypedField(args.update, StringData("$set"), Object, &setElement);
        if (setStatus.isOK()) {
            BSONObj setField = setElement.Obj();

            if (setField.hasField(ShardDatabaseType::enterCriticalSectionCounter.name())) {
                AutoGetDb autoDb(opCtx, db, MODE_X);
                if (autoDb.getDb()) {
                    DatabaseShardingState::get(autoDb.getDb()).setDbVersion(opCtx, boost::none);
                }
            }
        }
    }

    if (metadata) {
        incrementChunkOnInsertOrUpdate(
            opCtx, *metadata->getChunkManager(), args.updatedDoc, args.updatedDoc.objsize());
    }
}

void ShardServerOpObserver::aboutToDelete(OperationContext* opCtx,
                                          NamespaceString const& nss,
                                          BSONObj const& doc) {
    auto css = CollectionShardingState::get(opCtx, nss.ns());
    getDeleteState(opCtx) = ShardObserverDeleteState::make(opCtx, css, doc);
}

void ShardServerOpObserver::onDelete(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     OptionalCollectionUUID uuid,
                                     StmtId stmtId,
                                     bool fromMigrate,
                                     const boost::optional<BSONObj>& deletedDoc) {
    auto& deleteState = getDeleteState(opCtx);

    if (nss == NamespaceString::kShardConfigCollectionsNamespace) {
        onConfigDeleteInvalidateCachedCollectionMetadataAndNotify(opCtx, deleteState.documentKey);
    }
    if (nss == NamespaceString::kShardConfigDatabasesNamespace) {
        if (isStandaloneOrPrimary(opCtx)) {
            return;
        }

        // Extract which database entry is being deleted from the _id field.
        std::string deletedDatabase;
        fassert(50772,
                bsonExtractStringField(
                    deleteState.documentKey, ShardDatabaseType::name.name(), &deletedDatabase));

        AutoGetDb autoDb(opCtx, deletedDatabase, MODE_X);
        if (autoDb.getDb()) {
            DatabaseShardingState::get(autoDb.getDb()).setDbVersion(opCtx, boost::none);
        }
    }

    if (nss == NamespaceString::kServerConfigurationNamespace) {
        if (auto idElem = deleteState.documentKey["_id"]) {
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

repl::OpTime ShardServerOpObserver::onDropCollection(OperationContext* opCtx,
                                                     const NamespaceString& collectionName,
                                                     OptionalCollectionUUID uuid) {
    if (collectionName == NamespaceString::kServerConfigurationNamespace) {
        // Dropping system collections is not allowed for end users
        invariant(!opCtx->writesAreReplicated());
        invariant(repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback());

        // Can't confirm whether there was a ShardIdentity document or not yet, so assume there was
        // one and shut down the process to clear the in-memory sharding state
        warning() << "admin.system.version collection rolled back. Will shut down after finishing "
                     "rollback";

        ShardIdentityRollbackNotifier::get(opCtx)->recordThatRollbackHappened();
    }

    return {};
}

void shardObserveInsertOp(OperationContext* opCtx,
                          CollectionShardingState* css,
                          const BSONObj& insertedDoc,
                          const repl::OpTime& opTime) {
    css->checkShardVersionOrThrow(opCtx);
    auto msm = MigrationSourceManager::get(css);
    if (msm) {
        msm->getCloner()->onInsertOp(opCtx, insertedDoc, opTime);
    }
}

void shardObserveUpdateOp(OperationContext* opCtx,
                          CollectionShardingState* css,
                          const BSONObj& updatedDoc,
                          const repl::OpTime& opTime,
                          const repl::OpTime& prePostImageOpTime) {
    css->checkShardVersionOrThrow(opCtx);
    auto msm = MigrationSourceManager::get(css);
    if (msm) {
        msm->getCloner()->onUpdateOp(opCtx, updatedDoc, opTime, prePostImageOpTime);
    }
}

void shardObserveDeleteOp(OperationContext* opCtx,
                          CollectionShardingState* css,
                          const ShardObserverDeleteState& deleteState,
                          const repl::OpTime& opTime,
                          const repl::OpTime& preImageOpTime) {
    css->checkShardVersionOrThrow(opCtx);
    auto msm = MigrationSourceManager::get(css);
    if (msm && deleteState.isMigrating) {
        msm->getCloner()->onDeleteOp(opCtx, deleteState.documentKey, opTime, preImageOpTime);
    }
}

ShardObserverDeleteState ShardObserverDeleteState::make(OperationContext* opCtx,
                                                        CollectionShardingState* css,
                                                        const BSONObj& docToDelete) {
    auto msm = MigrationSourceManager::get(css);
    return {css->getMetadata(opCtx).extractDocumentKey(docToDelete).getOwned(),
            msm && msm->getCloner()->isDocumentInMigratingChunk(docToDelete)};
}

}  // namespace mongo

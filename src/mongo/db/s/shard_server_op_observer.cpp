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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_server_op_observer.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/s/chunk_split_state_driver.h"
#include "mongo/db/s/chunk_splitter.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/catalog/type_shard_database.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto getDocumentKey = OperationContext::declareDecoration<BSONObj>();

bool isStandaloneOrPrimary(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    return !isReplSet ||
        (repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
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
        invariant(_opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_IX));

        CatalogCacheLoader::get(_opCtx).notifyOfCollectionVersionUpdate(_nss);

        // Force subsequent uses of the namespace to refresh the filtering metadata so they can
        // synchronize with any work happening on the primary (e.g., migration critical section).
        CollectionShardingRuntime::get(_opCtx, _nss)->clearFilteringMetadata();
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
        try {
            ShardingInitializationMongoD::get(_opCtx)->initializeFromShardIdentity(_opCtx,
                                                                                   _shardIdentity);
        } catch (const AssertionException& ex) {
            fassertFailedWithStatus(40071, ex.toStatus());
        }
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
            bsonExtractStringField(query, ShardCollectionType::kNssFieldName, &deletedCollection));
    const NamespaceString deletedNss(deletedCollection);

    // Need the WUOW to retain the lock for CollectionVersionLogOpHandler::commit().
    AutoGetCollection autoColl(opCtx, deletedNss, MODE_IX);

    opCtx->recoveryUnit()->registerChange(new CollectionVersionLogOpHandler(opCtx, deletedNss));
}

/**
 * If the collection is sharded, finds the chunk that contains the specified document and increments
 * the size tracked for that chunk by the specified amount of data written, in bytes. Returns the
 * number of total bytes on that chunk after the data is written.
 */
void incrementChunkOnInsertOrUpdate(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const ChunkManager& chunkManager,
                                    const BSONObj& document,
                                    long dataWritten,
                                    bool fromMigrate) {
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
    auto chunkWritesTracker = chunk.getWritesTracker();
    chunkWritesTracker->addBytesWritten(dataWritten);
    // Don't trigger chunk splits from inserts happening due to migration since
    // we don't necessarily own that chunk yet
    if (!fromMigrate) {
        const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();

        if (balancerConfig->getShouldAutoSplit() &&
            chunkWritesTracker->shouldSplit(balancerConfig->getMaxChunkSizeBytes())) {
            auto chunkSplitStateDriver =
                ChunkSplitStateDriver::tryInitiateSplit(chunkWritesTracker);
            if (chunkSplitStateDriver) {
                ChunkSplitter::get(opCtx).trySplitting(std::move(chunkSplitStateDriver),
                                                       nss,
                                                       chunk.getMin(),
                                                       chunk.getMax(),
                                                       dataWritten);
            }
        }
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
    auto* const css = CollectionShardingState::get(opCtx, nss);
    const auto metadata = css->getCurrentMetadata();

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

        if (metadata->isSharded()) {
            incrementChunkOnInsertOrUpdate(opCtx,
                                           nss,
                                           *metadata->getChunkManager(),
                                           insertedDoc,
                                           insertedDoc.objsize(),
                                           fromMigrate);
        }
    }
}

void ShardServerOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    auto* const css = CollectionShardingState::get(opCtx, args.nss);
    const auto metadata = css->getCurrentMetadata();

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
                    bsonExtractStringField(
                        args.updateArgs.criteria, ShardCollectionType::kNssFieldName, &coll));
            return NamespaceString(coll);
        }());

        // Parse the '$set' update
        BSONElement setElement;
        Status setStatus =
            bsonExtractTypedField(args.updateArgs.update, StringData("$set"), Object, &setElement);
        if (setStatus.isOK()) {
            BSONObj setField = setElement.Obj();

            // Need the WUOW to retain the lock for CollectionVersionLogOpHandler::commit()
            AutoGetCollection autoColl(opCtx, updatedNss, MODE_IX);

            if (setField.hasField(ShardCollectionType::kLastRefreshedCollectionVersionFieldName) &&
                !setField.getBoolField("refreshing")) {
                opCtx->recoveryUnit()->registerChange(
                    new CollectionVersionLogOpHandler(opCtx, updatedNss));
            }

            if (setField.hasField(ShardCollectionType::kEnterCriticalSectionCounterFieldName)) {
                // Force subsequent uses of the namespace to refresh the filtering metadata so they
                // can synchronize with any work happening on the primary (e.g., migration critical
                // section).
                CollectionShardingRuntime::get(opCtx, updatedNss)->clearFilteringMetadata();
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
        fassert(
            40478,
            bsonExtractStringField(args.updateArgs.criteria, ShardDatabaseType::name.name(), &db));

        // Parse the '$set' update
        BSONElement setElement;
        Status setStatus =
            bsonExtractTypedField(args.updateArgs.update, StringData("$set"), Object, &setElement);
        if (setStatus.isOK()) {
            BSONObj setField = setElement.Obj();

            if (setField.hasField(ShardDatabaseType::enterCriticalSectionCounter.name())) {
                AutoGetDb autoDb(opCtx, db, MODE_X);
                auto dss = DatabaseShardingState::get(opCtx, db);
                auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);
                dss->setDbVersion(opCtx, boost::none, dssLock);
            }
        }
    }

    if (metadata->isSharded()) {
        incrementChunkOnInsertOrUpdate(opCtx,
                                       args.nss,
                                       *metadata->getChunkManager(),
                                       args.updateArgs.updatedDoc,
                                       args.updateArgs.updatedDoc.objsize(),
                                       args.updateArgs.fromMigrate);
    }
}

void ShardServerOpObserver::aboutToDelete(OperationContext* opCtx,
                                          NamespaceString const& nss,
                                          BSONObj const& doc) {
    getDocumentKey(opCtx) = OpObserverImpl::getDocumentKey(opCtx, nss, doc);
}

void ShardServerOpObserver::onDelete(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     OptionalCollectionUUID uuid,
                                     StmtId stmtId,
                                     bool fromMigrate,
                                     const boost::optional<BSONObj>& deletedDoc) {
    auto& documentKey = getDocumentKey(opCtx);

    if (nss == NamespaceString::kShardConfigCollectionsNamespace) {
        onConfigDeleteInvalidateCachedCollectionMetadataAndNotify(opCtx, documentKey);
    }
    if (nss == NamespaceString::kShardConfigDatabasesNamespace) {
        if (isStandaloneOrPrimary(opCtx)) {
            return;
        }

        // Extract which database entry is being deleted from the _id field.
        std::string deletedDatabase;
        fassert(
            50772,
            bsonExtractStringField(documentKey, ShardDatabaseType::name.name(), &deletedDatabase));

        AutoGetDb autoDb(opCtx, deletedDatabase, MODE_X);
        auto dss = DatabaseShardingState::get(opCtx, deletedDatabase);
        auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);
        dss->setDbVersion(opCtx, boost::none, dssLock);
    }

    if (nss == NamespaceString::kServerConfigurationNamespace) {
        if (auto idElem = documentKey["_id"]) {
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
                                                     OptionalCollectionUUID uuid,
                                                     std::uint64_t numRecords,
                                                     const CollectionDropType dropType) {
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

}  // namespace mongo

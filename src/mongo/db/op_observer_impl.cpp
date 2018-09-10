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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/op_observer_impl.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace {

MONGO_FP_DECLARE(failCollectionUpdates);

/**
 * Returns whether we're a master using master-slave replication.
 */
bool isMasterSlave(OperationContext* opCtx) {
    return repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() ==
        repl::ReplicationCoordinator::modeMasterSlave;
}

/**
 * Updates the session state with the last write timestamp and transaction for that session.
 *
 * In the case of writes with transaction/statement id, this method will be recursively entered a
 * second time for the actual write to the transactions table. Since this write does not generate an
 * oplog entry, the recursion will stop at this point.
 */
void onWriteOpCompleted(OperationContext* opCtx,
                        const NamespaceString& nss,
                        Session* session,
                        std::vector<StmtId> stmtIdsWritten,
                        const repl::OpTime& lastStmtIdWriteOpTime,
                        Date_t lastStmtIdWriteDate) {
    if (lastStmtIdWriteOpTime.isNull())
        return;

    if (session) {
        session->onWriteOpCompletedOnPrimary(opCtx,
                                             *opCtx->getTxnNumber(),
                                             std::move(stmtIdsWritten),
                                             lastStmtIdWriteOpTime,
                                             lastStmtIdWriteDate);
    }
}

/**
 * Given a raw collMod command object and associated collection metadata, create and return the
 * object for the 'o' field of a collMod oplog entry. For TTL index updates, we make sure the oplog
 * entry always stores the index name, instead of a key pattern.
 */
BSONObj makeCollModCmdObj(const BSONObj& collModCmd,
                          const CollectionOptions& oldCollOptions,
                          boost::optional<TTLCollModInfo> ttlInfo) {
    BSONObjBuilder cmdObjBuilder;
    std::string ttlIndexFieldName = "index";

    // Add all fields from the original collMod command.
    for (auto elem : collModCmd) {
        // We normalize all TTL collMod oplog entry objects to use the index name, even if the
        // command used an index key pattern.
        if (elem.fieldNameStringData() == ttlIndexFieldName && ttlInfo) {
            BSONObjBuilder ttlIndexObjBuilder;
            ttlIndexObjBuilder.append("name", ttlInfo->indexName);
            ttlIndexObjBuilder.append("expireAfterSeconds",
                                      durationCount<Seconds>(ttlInfo->expireAfterSeconds));

            cmdObjBuilder.append(ttlIndexFieldName, ttlIndexObjBuilder.obj());
        } else {
            cmdObjBuilder.append(elem);
        }
    }

    return cmdObjBuilder.obj();
}

Date_t getWallClockTimeForOpLog(OperationContext* opCtx) {
    auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
    return clockSource->now();
}

struct OpTimeBundle {
    repl::OpTime writeOpTime;
    repl::OpTime prePostImageOpTime;
    Date_t wallClockTime;
};

/**
 * Write oplog entry(ies) for the update operation.
 */
OpTimeBundle replLogUpdate(OperationContext* opCtx,
                           Session* session,
                           const OplogUpdateEntryArgs& args) {
    BSONObj storeObj;
    if (args.storeDocOption == OplogUpdateEntryArgs::StoreDocOption::PreImage) {
        invariant(args.preImageDoc);
        storeObj = *args.preImageDoc;
    } else if (args.storeDocOption == OplogUpdateEntryArgs::StoreDocOption::PostImage) {
        storeObj = args.updatedDoc;
    }

    OperationSessionInfo sessionInfo;
    repl::OplogLink oplogLink;

    if (session) {
        sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
        sessionInfo.setTxnNumber(*opCtx->getTxnNumber());
        oplogLink.prevOpTime = session->getLastWriteOpTime(*opCtx->getTxnNumber());
    }

    OpTimeBundle opTimes;
    opTimes.wallClockTime = getWallClockTimeForOpLog(opCtx);

    if (!storeObj.isEmpty() && opCtx->getTxnNumber()) {
        auto noteUpdateOpTime = repl::logOp(opCtx,
                                            "n",
                                            args.nss,
                                            args.uuid,
                                            storeObj,
                                            nullptr,
                                            false,
                                            opTimes.wallClockTime,
                                            sessionInfo,
                                            args.stmtId,
                                            {},
                                            OplogSlot());

        opTimes.prePostImageOpTime = noteUpdateOpTime;

        if (args.storeDocOption == OplogUpdateEntryArgs::StoreDocOption::PreImage) {
            oplogLink.preImageOpTime = noteUpdateOpTime;
        } else if (args.storeDocOption == OplogUpdateEntryArgs::StoreDocOption::PostImage) {
            oplogLink.postImageOpTime = noteUpdateOpTime;
        }
    }

    opTimes.writeOpTime = repl::logOp(opCtx,
                                      "u",
                                      args.nss,
                                      args.uuid,
                                      args.update,
                                      &args.criteria,
                                      args.fromMigrate,
                                      opTimes.wallClockTime,
                                      sessionInfo,
                                      args.stmtId,
                                      oplogLink,
                                      OplogSlot());

    return opTimes;
}

/**
 * Write oplog entry(ies) for the delete operation.
 */
OpTimeBundle replLogDelete(OperationContext* opCtx,
                           const NamespaceString& nss,
                           OptionalCollectionUUID uuid,
                           Session* session,
                           StmtId stmtId,
                           const CollectionShardingState::DeleteState& deleteState,
                           bool fromMigrate,
                           const boost::optional<BSONObj>& deletedDoc) {
    OperationSessionInfo sessionInfo;
    repl::OplogLink oplogLink;

    if (session) {
        sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
        sessionInfo.setTxnNumber(*opCtx->getTxnNumber());
        oplogLink.prevOpTime = session->getLastWriteOpTime(*opCtx->getTxnNumber());
    }

    OpTimeBundle opTimes;
    opTimes.wallClockTime = getWallClockTimeForOpLog(opCtx);

    if (deletedDoc && opCtx->getTxnNumber()) {
        auto noteOplog = repl::logOp(opCtx,
                                     "n",
                                     nss,
                                     uuid,
                                     deletedDoc.get(),
                                     nullptr,
                                     false,
                                     opTimes.wallClockTime,
                                     sessionInfo,
                                     stmtId,
                                     {},
                                     OplogSlot());
        opTimes.prePostImageOpTime = noteOplog;
        oplogLink.preImageOpTime = noteOplog;
    }

    opTimes.writeOpTime = repl::logOp(opCtx,
                                      "d",
                                      nss,
                                      uuid,
                                      deleteState.documentKey,
                                      nullptr,
                                      fromMigrate,
                                      opTimes.wallClockTime,
                                      sessionInfo,
                                      stmtId,
                                      oplogLink,
                                      OplogSlot());
    return opTimes;
}

}  // namespace

void OpObserverImpl::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   OptionalCollectionUUID uuid,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
    const NamespaceString systemIndexes{nss.getSystemIndexesCollection()};

    if (uuid && !isMasterSlave(opCtx)) {
        BSONObjBuilder builder;
        builder.append("createIndexes", nss.coll());

        for (const auto& e : indexDoc) {
            if (e.fieldNameStringData() != "ns"_sd)
                builder.append(e);
        }

        repl::logOp(opCtx,
                    "c",
                    nss.getCommandNS(),
                    uuid,
                    builder.done(),
                    nullptr,
                    fromMigrate,
                    getWallClockTimeForOpLog(opCtx),
                    {},
                    kUninitializedStmtId,
                    {},
                    OplogSlot());
    } else {
        repl::logOp(opCtx,
                    "i",
                    systemIndexes,
                    {},
                    indexDoc,
                    nullptr,
                    fromMigrate,
                    getWallClockTimeForOpLog(opCtx),
                    {},
                    kUninitializedStmtId,
                    {},
                    OplogSlot());
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "i", systemIndexes, indexDoc, nullptr);

    auto css = CollectionShardingState::get(opCtx, systemIndexes);
    if (!fromMigrate) {
        css->onInsertOp(opCtx, indexDoc, {});
    }
}

void OpObserverImpl::onInserts(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               std::vector<InsertStatement>::const_iterator begin,
                               std::vector<InsertStatement>::const_iterator end,
                               bool fromMigrate) {
    Session* const session = opCtx->getTxnNumber() ? OperationContextSession::get(opCtx) : nullptr;

    const auto lastWriteDate = getWallClockTimeForOpLog(opCtx);

    const auto opTimeList =
        repl::logInsertOps(opCtx, nss, uuid, session, begin, end, fromMigrate, lastWriteDate);

    auto css = (nss == NamespaceString::kSessionTransactionsTableNamespace || fromMigrate)
        ? nullptr
        : CollectionShardingState::get(opCtx, nss.ns());

    size_t index = 0;
    for (auto it = begin; it != end; it++, index++) {
        AuthorizationManager::get(opCtx->getServiceContext())
            ->logOp(opCtx, "i", nss, it->doc, nullptr);
        if (css) {
            auto opTime = opTimeList.empty() ? repl::OpTime() : opTimeList[index];
            css->onInsertOp(opCtx, it->doc, opTime);
        }
    }

    const auto lastOpTime = opTimeList.empty() ? repl::OpTime() : opTimeList.back();
    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    } else if (nss.ns() == FeatureCompatibilityVersion::kCollection) {
        for (auto it = begin; it != end; it++) {
            FeatureCompatibilityVersion::onInsertOrUpdate(opCtx, it->doc);
        }
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace && !lastOpTime.isNull()) {
        for (auto it = begin; it != end; it++) {
            SessionCatalog::get(opCtx)->invalidateSessions(opCtx, it->doc);
        }
    }

    std::vector<StmtId> stmtIdsWritten;
    std::transform(begin, end, std::back_inserter(stmtIdsWritten), [](const InsertStatement& stmt) {
        return stmt.stmtId;
    });

    onWriteOpCompleted(opCtx, nss, session, stmtIdsWritten, lastOpTime, lastWriteDate);
}

void OpObserverImpl::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    MONGO_FAIL_POINT_BLOCK(failCollectionUpdates, extraData) {
        auto collElem = extraData.getData()["collectionNS"];
        // If the failpoint specifies no collection or matches the existing one, fail.
        if (!collElem || args.nss.ns() == collElem.String()) {
            uasserted(40654,
                      str::stream() << "failCollectionUpdates failpoint enabled, namespace: "
                                    << args.nss.ns()
                                    << ", update: "
                                    << args.update
                                    << " on document with "
                                    << args.criteria);
        }
    }

    // Do not log a no-op operation; see SERVER-21738
    if (args.update.isEmpty()) {
        return;
    }

    Session* const session = opCtx->getTxnNumber() ? OperationContextSession::get(opCtx) : nullptr;
    const auto opTime = replLogUpdate(opCtx, session, args);

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "u", args.nss, args.update, &args.criteria);

    if (args.nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!args.fromMigrate) {
            auto css = CollectionShardingState::get(opCtx, args.nss);
            css->onUpdateOp(opCtx,
                            args.criteria,
                            args.update,
                            args.updatedDoc,
                            opTime.writeOpTime,
                            opTime.prePostImageOpTime);
        }
    }

    if (args.nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (args.nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, args.nss);
    } else if (args.nss.ns() == FeatureCompatibilityVersion::kCollection) {
        FeatureCompatibilityVersion::onInsertOrUpdate(opCtx, args.updatedDoc);
    } else if (args.nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        SessionCatalog::get(opCtx)->invalidateSessions(opCtx, args.updatedDoc);
    }

    onWriteOpCompleted(opCtx,
                       args.nss,
                       session,
                       std::vector<StmtId>{args.stmtId},
                       opTime.writeOpTime,
                       opTime.wallClockTime);
}

auto OpObserverImpl::aboutToDelete(OperationContext* opCtx,
                                   NamespaceString const& nss,
                                   BSONObj const& doc) -> CollectionShardingState::DeleteState {
    auto* css = CollectionShardingState::get(opCtx, nss.ns());
    return css->makeDeleteState(doc);
}

void OpObserverImpl::onDelete(OperationContext* opCtx,
                              const NamespaceString& nss,
                              OptionalCollectionUUID uuid,
                              StmtId stmtId,
                              CollectionShardingState::DeleteState deleteState,
                              bool fromMigrate,
                              const boost::optional<BSONObj>& deletedDoc) {
    if (deleteState.documentKey.isEmpty()) {
        return;
    }

    Session* const session = opCtx->getTxnNumber() ? OperationContextSession::get(opCtx) : nullptr;
    const auto opTime =
        replLogDelete(opCtx, nss, uuid, session, stmtId, deleteState, fromMigrate, deletedDoc);

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "d", nss, deleteState.documentKey, nullptr);

    if (nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!fromMigrate) {
            auto css = CollectionShardingState::get(opCtx, nss.ns());
            css->onDeleteOp(opCtx, deleteState, opTime.writeOpTime, opTime.prePostImageOpTime);
        }
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    } else if (nss.isAdminDotSystemDotVersion()) {
        auto _id = deleteState.documentKey["_id"];
        if (_id.type() == BSONType::String &&
            _id.String() == FeatureCompatibilityVersion::kParameterName)
            uasserted(40670, "removing FeatureCompatibilityVersion document is not allowed");
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        SessionCatalog::get(opCtx)->invalidateSessions(opCtx, deleteState.documentKey);
    }

    onWriteOpCompleted(
        opCtx, nss, session, std::vector<StmtId>{stmtId}, opTime.writeOpTime, opTime.wallClockTime);
}

void OpObserverImpl::onInternalOpMessage(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const boost::optional<UUID> uuid,
                                         const BSONObj& msgObj,
                                         const boost::optional<BSONObj> o2MsgObj) {
    const BSONObj* o2MsgPtr = o2MsgObj ? o2MsgObj.get_ptr() : nullptr;
    repl::logOp(opCtx,
                "n",
                nss,
                uuid,
                msgObj,
                o2MsgPtr,
                false,
                getWallClockTimeForOpLog(opCtx),
                {},
                kUninitializedStmtId,
                {},
                OplogSlot());
}

void OpObserverImpl::onCreateCollection(OperationContext* opCtx,
                                        Collection* coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex,
                                        const OplogSlot& createOpTime) {
    const auto cmdNss = collectionName.getCommandNS();

    BSONObjBuilder b;
    b.append("create", collectionName.coll().toString());
    {
        // Don't store the UUID as part of the options, but instead only at the top level
        CollectionOptions optionsToStore = options;
        optionsToStore.uuid.reset();
        b.appendElements(optionsToStore.toBSON());
    }

    // Include the full _id index spec in the oplog for index versions >= 2.
    if (!idIndex.isEmpty()) {
        auto versionElem = idIndex[IndexDescriptor::kIndexVersionFieldName];
        invariant(versionElem.isNumber());
        if (IndexDescriptor::IndexVersion::kV2 <=
            static_cast<IndexDescriptor::IndexVersion>(versionElem.numberInt())) {
            b.append("idIndex", idIndex);
        }
    }

    const auto cmdObj = b.done();

    if (!collectionName.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(opCtx,
                    "c",
                    cmdNss,
                    options.uuid,
                    cmdObj,
                    nullptr,
                    false,
                    getWallClockTimeForOpLog(opCtx),
                    {},
                    kUninitializedStmtId,
                    {},
                    OplogSlot());
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

    if (options.uuid) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        catalog.onCreateCollection(opCtx, coll, options.uuid.get());
        opCtx->recoveryUnit()->onRollback([opCtx, collectionName]() {
            NamespaceUUIDCache::get(opCtx).evictNamespace(collectionName);
        });
    }
}

void OpObserverImpl::onCollMod(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               const BSONObj& collModCmd,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<TTLCollModInfo> ttlInfo) {
    const auto cmdNss = nss.getCommandNS();

    // Create the 'o' field object.
    const auto cmdObj = makeCollModCmdObj(collModCmd, oldCollOptions, ttlInfo);

    // Create the 'o2' field object. We save the old collection metadata and TTL expiration.
    BSONObjBuilder o2Builder;
    o2Builder.append("collectionOptions_old", oldCollOptions.toBSON());
    if (ttlInfo) {
        auto oldExpireAfterSeconds = durationCount<Seconds>(ttlInfo->oldExpireAfterSeconds);
        o2Builder.append("expireAfterSeconds_old", oldExpireAfterSeconds);
    }

    const auto o2Obj = o2Builder.done();

    if (!nss.isSystemDotProfile()) {
        // do not replicate system.profile modifications
        repl::logOp(opCtx,
                    "c",
                    cmdNss,
                    uuid,
                    cmdObj,
                    &o2Obj,
                    false,
                    getWallClockTimeForOpLog(opCtx),
                    {},
                    kUninitializedStmtId,
                    {},
                    OplogSlot());
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

    // Make sure the UUID values in the Collection metadata, the Collection object, and the UUID
    // catalog are all present and equal if uuid exists and do not exist if uuid does not exist.
    invariant(opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    Database* db = dbHolder().get(opCtx, nss.db());
    // Some unit tests call the op observer on an unregistered Database.
    if (!db) {
        return;
    }
    Collection* coll = db->getCollection(opCtx, nss.ns());
    invariant(coll->uuid() == uuid,
              str::stream() << (uuid ? uuid->toString() : "<no uuid>") << ","
                            << (coll->uuid() ? coll->uuid()->toString() : "<no uuid>"));
    CollectionCatalogEntry* entry = coll->getCatalogEntry();
    invariant(entry->isEqualToMetadataUUID(opCtx, uuid));

    if (uuid) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx->getServiceContext());
        Collection* catalogColl = catalog.lookupCollectionByUUID(uuid.get());
        invariant(catalogColl && catalogColl->uuid() == uuid);
    }
}

void OpObserverImpl::onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
    const NamespaceString cmdNss{dbName, "$cmd"};
    const auto cmdObj = BSON("dropDatabase" << 1);

    repl::logOp(opCtx,
                "c",
                cmdNss,
                {},
                cmdObj,
                nullptr,
                false,
                getWallClockTimeForOpLog(opCtx),
                {},
                kUninitializedStmtId,
                {},
                OplogSlot());

    if (dbName == FeatureCompatibilityVersion::kDatabase) {
        FeatureCompatibilityVersion::onDropCollection(opCtx);
    } else if (dbName == NamespaceString::kSessionTransactionsTableNamespace.db()) {
        SessionCatalog::get(opCtx)->invalidateSessions(opCtx, boost::none);
    }

    NamespaceUUIDCache::get(opCtx).evictNamespacesInDatabase(dbName);

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

repl::OpTime OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid) {
    const auto cmdNss = collectionName.getCommandNS();
    const auto cmdObj = BSON("drop" << collectionName.coll());

    repl::OpTime dropOpTime;
    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications
        dropOpTime = repl::logOp(opCtx,
                                 "c",
                                 cmdNss,
                                 uuid,
                                 cmdObj,
                                 nullptr,
                                 false,
                                 getWallClockTimeForOpLog(opCtx),
                                 {},
                                 kUninitializedStmtId,
                                 {},
                                 OplogSlot());
    }

    if (collectionName.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, collectionName);
    } else if (collectionName.ns() == FeatureCompatibilityVersion::kCollection) {
        FeatureCompatibilityVersion::onDropCollection(opCtx);
    } else if (collectionName == NamespaceString::kSessionTransactionsTableNamespace) {
        SessionCatalog::get(opCtx)->invalidateSessions(opCtx, boost::none);
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

    auto css = CollectionShardingState::get(opCtx, collectionName);
    css->onDropCollection(opCtx, collectionName);

    // Evict namespace entry from the namespace/uuid cache if it exists.
    NamespaceUUIDCache::get(opCtx).evictNamespace(collectionName);

    // Remove collection from the uuid catalog.
    if (uuid) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        catalog.onDropCollection(opCtx, uuid.get());
    }

    return dropOpTime;
}

void OpObserverImpl::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OptionalCollectionUUID uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    const auto cmdNss = nss.getCommandNS();
    const auto cmdObj = BSON("dropIndexes" << nss.coll() << "index" << indexName);

    repl::logOp(opCtx,
                "c",
                cmdNss,
                uuid,
                cmdObj,
                &indexInfo,
                false,
                getWallClockTimeForOpLog(opCtx),
                {},
                kUninitializedStmtId,
                {},
                OplogSlot());

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, &indexInfo);
}

repl::OpTime OpObserverImpl::onRenameCollection(OperationContext* opCtx,
                                                const NamespaceString& fromCollection,
                                                const NamespaceString& toCollection,
                                                OptionalCollectionUUID uuid,
                                                bool dropTarget,
                                                OptionalCollectionUUID dropTargetUUID,
                                                bool stayTemp) {
    const auto cmdNss = fromCollection.getCommandNS();

    BSONObjBuilder builder;
    builder.append("renameCollection", fromCollection.ns());
    builder.append("to", toCollection.ns());
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID && enableCollectionUUIDs && !isMasterSlave(opCtx)) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    } else {
        builder.append("dropTarget", dropTarget);
    }

    const auto cmdObj = builder.done();

    const auto renameOpTime = repl::logOp(opCtx,
                                          "c",
                                          cmdNss,
                                          uuid,
                                          cmdObj,
                                          nullptr,
                                          false,
                                          getWallClockTimeForOpLog(opCtx),
                                          {},
                                          kUninitializedStmtId,
                                          {},
                                          OplogSlot());

    if (fromCollection.isSystemDotViews())
        DurableViewCatalog::onExternalChange(opCtx, fromCollection);
    if (toCollection.isSystemDotViews())
        DurableViewCatalog::onExternalChange(opCtx, toCollection);

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

    // Evict namespace entry from the namespace/uuid cache if it exists.
    NamespaceUUIDCache& cache = NamespaceUUIDCache::get(opCtx);
    cache.evictNamespace(fromCollection);
    cache.evictNamespace(toCollection);
    opCtx->recoveryUnit()->onRollback(
        [&cache, toCollection]() { cache.evictNamespace(toCollection); });

    // Finally update the UUID Catalog.
    if (uuid) {
        auto getNewCollection = [opCtx, toCollection] {
            auto db = dbHolder().get(opCtx, toCollection.db());
            auto newColl = db->getCollection(opCtx, toCollection);
            invariant(newColl);
            return newColl;
        };
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        catalog.onRenameCollection(opCtx, getNewCollection, uuid.get());
    }

    return renameOpTime;
}

void OpObserverImpl::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
    const NamespaceString cmdNss{dbName, "$cmd"};
    repl::logOp(opCtx,
                "c",
                cmdNss,
                {},
                applyOpCmd,
                nullptr,
                false,
                getWallClockTimeForOpLog(opCtx),
                {},
                kUninitializedStmtId,
                {},
                OplogSlot());

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, applyOpCmd, nullptr);
}

void OpObserverImpl::onEmptyCapped(OperationContext* opCtx,
                                   const NamespaceString& collectionName,
                                   OptionalCollectionUUID uuid) {
    const auto cmdNss = collectionName.getCommandNS();
    const auto cmdObj = BSON("emptycapped" << collectionName.coll());

    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications
        repl::logOp(opCtx,
                    "c",
                    cmdNss,
                    uuid,
                    cmdObj,
                    nullptr,
                    false,
                    getWallClockTimeForOpLog(opCtx),
                    {},
                    kUninitializedStmtId,
                    {},
                    OplogSlot());
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

}  // namespace mongo

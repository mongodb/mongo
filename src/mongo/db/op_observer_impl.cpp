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
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
using repl::OplogEntry;
namespace {

MONGO_FAIL_POINT_DEFINE(failCollectionUpdates);

const auto getDeleteState = OperationContext::declareDecoration<ShardObserverDeleteState>();

repl::OpTime logOperation(OperationContext* opCtx,
                          const char* opstr,
                          const NamespaceString& ns,
                          OptionalCollectionUUID uuid,
                          const BSONObj& obj,
                          const BSONObj* o2,
                          bool fromMigrate,
                          Date_t wallClockTime,
                          const OperationSessionInfo& sessionInfo,
                          StmtId stmtId,
                          const repl::OplogLink& oplogLink,
                          bool prepare) {
    auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
    auto opTime = repl::logOp(opCtx,
                              opstr,
                              ns,
                              uuid,
                              obj,
                              o2,
                              fromMigrate,
                              wallClockTime,
                              sessionInfo,
                              stmtId,
                              oplogLink,
                              prepare);

    times.push_back(opTime);
    return opTime;
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
        auto noteUpdateOpTime = logOperation(opCtx,
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
                                             false /* prepare */);

        opTimes.prePostImageOpTime = noteUpdateOpTime;

        if (args.storeDocOption == OplogUpdateEntryArgs::StoreDocOption::PreImage) {
            oplogLink.preImageOpTime = noteUpdateOpTime;
        } else if (args.storeDocOption == OplogUpdateEntryArgs::StoreDocOption::PostImage) {
            oplogLink.postImageOpTime = noteUpdateOpTime;
        }
    }

    opTimes.writeOpTime = logOperation(opCtx,
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
                                       false /* prepare */);

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
        auto noteOplog = logOperation(opCtx,
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
                                      false /* prepare */);
        opTimes.prePostImageOpTime = noteOplog;
        oplogLink.preImageOpTime = noteOplog;
    }

    auto& deleteState = getDeleteState(opCtx);
    opTimes.writeOpTime = logOperation(opCtx,
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
                                       false /* prepare */);
    return opTimes;
}

/**
 * Write oplog entry for applyOps/atomic transaction operations.
 */
OpTimeBundle replLogApplyOps(OperationContext* opCtx,
                             const NamespaceString& cmdNss,
                             const BSONObj& applyOpCmd,
                             const OperationSessionInfo& sessionInfo,
                             StmtId stmtId,
                             const repl::OplogLink& oplogLink,
                             bool prepare) {
    OpTimeBundle times;
    times.wallClockTime = getWallClockTimeForOpLog(opCtx);
    times.writeOpTime = logOperation(opCtx,
                                     "c",
                                     cmdNss,
                                     {},
                                     applyOpCmd,
                                     nullptr,
                                     false,
                                     times.wallClockTime,
                                     sessionInfo,
                                     stmtId,
                                     oplogLink,
                                     prepare);
    return times;
}

}  // namespace

void OpObserverImpl::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   OptionalCollectionUUID uuid,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
    const NamespaceString systemIndexes{nss.getSystemIndexesCollection()};

    if (uuid) {
        BSONObjBuilder builder;
        builder.append("createIndexes", nss.coll());

        for (const auto& e : indexDoc) {
            if (e.fieldNameStringData() != "ns"_sd)
                builder.append(e);
        }

        logOperation(opCtx,
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
                     false /* prepare */);
    } else {
        logOperation(opCtx,
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
                     false /* prepare */);
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "i", systemIndexes, indexDoc, nullptr);
}

void OpObserverImpl::onInserts(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               std::vector<InsertStatement>::const_iterator first,
                               std::vector<InsertStatement>::const_iterator last,
                               bool fromMigrate) {
    Session* const session = OperationContextSession::get(opCtx);
    const bool inMultiDocumentTransaction =
        session && opCtx->writesAreReplicated() && session->inMultiDocumentTransaction();

    Date_t lastWriteDate;

    std::vector<repl::OpTime> opTimeList;
    repl::OpTime lastOpTime;

    if (inMultiDocumentTransaction) {
        // Do not add writes to the profile collection to the list of transaction operations, since
        // these are done outside the transaction.
        if (!opCtx->getWriteUnitOfWork()) {
            invariant(nss.isSystemDotProfile());
            return;
        }
        for (auto iter = first; iter != last; iter++) {
            auto operation = OplogEntry::makeInsertOperation(nss, uuid, iter->doc);
            session->addTransactionOperation(opCtx, operation);
        }
    } else {
        lastWriteDate = getWallClockTimeForOpLog(opCtx);

        opTimeList =
            repl::logInsertOps(opCtx, nss, uuid, session, first, last, fromMigrate, lastWriteDate);
        if (!opTimeList.empty())
            lastOpTime = opTimeList.back();

        auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
        using std::begin;
        using std::end;
        times.insert(end(times), begin(opTimeList), end(opTimeList));

        std::vector<StmtId> stmtIdsWritten;
        std::transform(first,
                       last,
                       std::back_inserter(stmtIdsWritten),
                       [](const InsertStatement& stmt) { return stmt.stmtId; });

        onWriteOpCompleted(opCtx, nss, session, stmtIdsWritten, lastOpTime, lastWriteDate);
    }

    auto css = (nss == NamespaceString::kSessionTransactionsTableNamespace || fromMigrate)
        ? nullptr
        : CollectionShardingState::get(opCtx, nss);

    size_t index = 0;
    for (auto it = first; it != last; it++, index++) {
        AuthorizationManager::get(opCtx->getServiceContext())
            ->logOp(opCtx, "i", nss, it->doc, nullptr);
        if (css) {
            auto opTime = opTimeList.empty() ? repl::OpTime() : opTimeList[index];
            shardObserveInsertOp(opCtx, css, it->doc, opTime);
        }
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    } else if (nss == NamespaceString::kServerConfigurationNamespace) {
        // We must check server configuration collection writes for featureCompatibilityVersion
        // document changes.
        for (auto it = first; it != last; it++) {
            FeatureCompatibilityVersion::onInsertOrUpdate(opCtx, it->doc);
        }
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace && !lastOpTime.isNull()) {
        for (auto it = first; it != last; it++) {
            SessionCatalog::get(opCtx)->invalidateSessions(opCtx, it->doc);
        }
    }
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

    Session* const session = OperationContextSession::get(opCtx);
    const bool inMultiDocumentTransaction =
        session && opCtx->writesAreReplicated() && session->inMultiDocumentTransaction();
    OpTimeBundle opTime;
    if (inMultiDocumentTransaction) {
        auto operation =
            OplogEntry::makeUpdateOperation(args.nss, args.uuid, args.update, args.criteria);
        session->addTransactionOperation(opCtx, operation);
    } else {
        opTime = replLogUpdate(opCtx, session, args);
        onWriteOpCompleted(opCtx,
                           args.nss,
                           session,
                           std::vector<StmtId>{args.stmtId},
                           opTime.writeOpTime,
                           opTime.wallClockTime);
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "u", args.nss, args.update, &args.criteria);

    if (args.nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!args.fromMigrate) {
            auto css = CollectionShardingState::get(opCtx, args.nss);
            shardObserveUpdateOp(
                opCtx, css, args.updatedDoc, opTime.writeOpTime, opTime.prePostImageOpTime);
        }
    }

    if (args.nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (args.nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, args.nss);
    } else if (args.nss == NamespaceString::kServerConfigurationNamespace) {
        // We must check server configuration collection writes for featureCompatibilityVersion
        // document changes.
        FeatureCompatibilityVersion::onInsertOrUpdate(opCtx, args.updatedDoc);
    } else if (args.nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        SessionCatalog::get(opCtx)->invalidateSessions(opCtx, args.updatedDoc);
    }
}

void OpObserverImpl::aboutToDelete(OperationContext* opCtx,
                                   NamespaceString const& nss,
                                   BSONObj const& doc) {
    getDeleteState(opCtx) =
        ShardObserverDeleteState::make(opCtx, CollectionShardingState::get(opCtx, nss), doc);
}

void OpObserverImpl::onDelete(OperationContext* opCtx,
                              const NamespaceString& nss,
                              OptionalCollectionUUID uuid,
                              StmtId stmtId,
                              bool fromMigrate,
                              const boost::optional<BSONObj>& deletedDoc) {
    Session* const session = OperationContextSession::get(opCtx);
    auto& deleteState = getDeleteState(opCtx);
    invariant(!deleteState.documentKey.isEmpty());
    const bool inMultiDocumentTransaction =
        session && opCtx->writesAreReplicated() && session->inMultiDocumentTransaction();
    OpTimeBundle opTime;
    if (inMultiDocumentTransaction) {
        auto operation = OplogEntry::makeDeleteOperation(
            nss, uuid, deletedDoc ? deletedDoc.get() : deleteState.documentKey);
        session->addTransactionOperation(opCtx, operation);
    } else {
        opTime = replLogDelete(opCtx, nss, uuid, session, stmtId, fromMigrate, deletedDoc);
        onWriteOpCompleted(opCtx,
                           nss,
                           session,
                           std::vector<StmtId>{stmtId},
                           opTime.writeOpTime,
                           opTime.wallClockTime);
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "d", nss, deleteState.documentKey, nullptr);

    if (nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!fromMigrate) {
            auto css = CollectionShardingState::get(opCtx, nss);
            shardObserveDeleteOp(
                opCtx, css, deleteState, opTime.writeOpTime, opTime.prePostImageOpTime);
        }
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, nss);
    } else if (nss.isServerConfigurationCollection()) {
        auto _id = deleteState.documentKey["_id"];
        if (_id.type() == BSONType::String &&
            _id.String() == FeatureCompatibilityVersionParser::kParameterName)
            uasserted(40670, "removing FeatureCompatibilityVersion document is not allowed");
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        SessionCatalog::get(opCtx)->invalidateSessions(opCtx, deleteState.documentKey);
    }
}

void OpObserverImpl::onInternalOpMessage(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const boost::optional<UUID> uuid,
                                         const BSONObj& msgObj,
                                         const boost::optional<BSONObj> o2MsgObj) {
    const BSONObj* o2MsgPtr = o2MsgObj ? o2MsgObj.get_ptr() : nullptr;
    logOperation(opCtx,
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
                 false /* prepare */);
}

void OpObserverImpl::onCreateCollection(OperationContext* opCtx,
                                        Collection* coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex) {
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
        logOperation(opCtx,
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
                     false /* prepare */);
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

    if (options.uuid) {
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
        logOperation(opCtx,
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
                     false /* prepare */);
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

    // Make sure the UUID values in the Collection metadata, the Collection object, and the UUID
    // catalog are all present and equal, unless the collection is system.indexes or
    // system.namespaces (see SERVER-29926, SERVER-30095).
    invariant(opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    Database* db = DatabaseHolder::getDatabaseHolder().get(opCtx, nss.db());
    // Some unit tests call the op observer on an unregistered Database.
    if (!db) {
        return;
    }
    Collection* coll = db->getCollection(opCtx, nss.ns());

    invariant(coll->uuid() || nss.coll() == "system.indexes" || nss.coll() == "system.namespaces");
    invariant(coll->uuid() == uuid);
    CollectionCatalogEntry* entry = coll->getCatalogEntry();
    invariant(entry->isEqualToMetadataUUID(opCtx, uuid));
}

void OpObserverImpl::onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
    const NamespaceString cmdNss{dbName, "$cmd"};
    const auto cmdObj = BSON("dropDatabase" << 1);

    logOperation(opCtx,
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
                 false /* prepare */);

    uassert(
        50714, "dropping the admin database is not allowed.", dbName != NamespaceString::kAdminDb);

    if (dbName == NamespaceString::kSessionTransactionsTableNamespace.db()) {
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

    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications.
        logOperation(opCtx,
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
                     false /* prepare */);
    }

    uassert(50715,
            "dropping the server configuration collection (admin.system.version) is not allowed.",
            collectionName != NamespaceString::kServerConfigurationNamespace);

    if (collectionName.coll() == DurableViewCatalog::viewsCollectionName()) {
        DurableViewCatalog::onExternalChange(opCtx, collectionName);
    } else if (collectionName == NamespaceString::kSessionTransactionsTableNamespace) {
        SessionCatalog::get(opCtx)->invalidateSessions(opCtx, boost::none);
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);

    // Evict namespace entry from the namespace/uuid cache if it exists.
    NamespaceUUIDCache::get(opCtx).evictNamespace(collectionName);

    return {};
}

void OpObserverImpl::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OptionalCollectionUUID uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    const auto cmdNss = nss.getCommandNS();
    const auto cmdObj = BSON("dropIndexes" << nss.coll() << "index" << indexName);

    logOperation(opCtx,
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
                 false /* prepare */);

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, &indexInfo);
}


repl::OpTime OpObserverImpl::preRenameCollection(OperationContext* const opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 OptionalCollectionUUID uuid,
                                                 OptionalCollectionUUID dropTargetUUID,
                                                 bool stayTemp) {
    const auto cmdNss = fromCollection.getCommandNS();

    BSONObjBuilder builder;
    builder.append("renameCollection", fromCollection.ns());
    builder.append("to", toCollection.ns());
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    }

    const auto cmdObj = builder.done();

    logOperation(opCtx,
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
                 false /* prepare */);

    return {};
}

void OpObserverImpl::postRenameCollection(OperationContext* const opCtx,
                                          const NamespaceString& fromCollection,
                                          const NamespaceString& toCollection,
                                          OptionalCollectionUUID uuid,
                                          OptionalCollectionUUID dropTargetUUID,
                                          bool stayTemp) {
    const auto cmdNss = fromCollection.getCommandNS();

    BSONObjBuilder builder;
    builder.append("renameCollection", fromCollection.ns());
    builder.append("to", toCollection.ns());
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    }

    const auto cmdObj = builder.done();

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
}

void OpObserverImpl::onRenameCollection(OperationContext* const opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        OptionalCollectionUUID uuid,
                                        OptionalCollectionUUID dropTargetUUID,
                                        bool stayTemp) {
    preRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    postRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
}

void OpObserverImpl::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& applyOpCmd) {
    const NamespaceString cmdNss{dbName, "$cmd"};

    // Only transactional 'applyOps' commands can be prepared.
    constexpr bool prepare = false;
    replLogApplyOps(opCtx, cmdNss, applyOpCmd, {}, kUninitializedStmtId, {}, prepare);

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
        logOperation(opCtx,
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
                     false /* prepare */);
    }

    AuthorizationManager::get(opCtx->getServiceContext())
        ->logOp(opCtx, "c", cmdNss, cmdObj, nullptr);
}

namespace {

OpTimeBundle logApplyOpsForTransaction(OperationContext* opCtx,
                                       Session* const session,
                                       std::vector<repl::ReplOperation> stmts,
                                       bool prepare) {
    BSONObjBuilder applyOpsBuilder;
    BSONArrayBuilder opsArray(applyOpsBuilder.subarrayStart("applyOps"_sd));
    for (auto& stmt : stmts) {
        opsArray.append(stmt.toBSON());
    }
    opsArray.done();

    const NamespaceString cmdNss{"admin", "$cmd"};

    OperationSessionInfo sessionInfo;
    repl::OplogLink oplogLink;
    sessionInfo.setSessionId(*opCtx->getLogicalSessionId());
    sessionInfo.setTxnNumber(*opCtx->getTxnNumber());
    StmtId stmtId(0);
    oplogLink.prevOpTime = session->getLastWriteOpTime(*opCtx->getTxnNumber());
    // Until we support multiple oplog entries per transaction, prevOpTime should always be null.
    invariant(oplogLink.prevOpTime.isNull());

    try {
        auto applyOpCmd = applyOpsBuilder.done();
        auto times =
            replLogApplyOps(opCtx, cmdNss, applyOpCmd, sessionInfo, stmtId, oplogLink, prepare);

        onWriteOpCompleted(
            opCtx, cmdNss, session, {stmtId}, times.writeOpTime, times.wallClockTime);
        return times;
    } catch (const AssertionException& e) {
        // Change the error code to TransactionTooLarge if it is BSONObjectTooLarge.
        uassert(ErrorCodes::TransactionTooLarge,
                e.reason(),
                e.code() != ErrorCodes::BSONObjectTooLarge);
        throw;
    }
    MONGO_UNREACHABLE;
}

}  //  namespace

void OpObserverImpl::onTransactionCommit(OperationContext* opCtx) {
    invariant(opCtx->getTxnNumber());
    Session* const session = OperationContextSession::get(opCtx);
    invariant(session);
    invariant(session->inMultiDocumentTransaction());
    auto stmts = session->endTransactionAndRetrieveOperations(opCtx);

    // It is possible that the transaction resulted in no changes.  In that case, we should
    // not write an empty applyOps entry.
    if (stmts.empty())
        return;

    const auto commitOpTime =
        logApplyOpsForTransaction(opCtx, session, stmts, false /* prepare */).writeOpTime;
    invariant(!commitOpTime.isNull());
}

void OpObserverImpl::onTransactionPrepare(OperationContext* opCtx) {
    invariant(opCtx->getTxnNumber());
    Session* const session = OperationContextSession::get(opCtx);
    invariant(session);
    invariant(session->inMultiDocumentTransaction());
    auto stmts = session->endTransactionAndRetrieveOperations(opCtx);

    // It is possible that the transaction resulted in no changes.  In that case, we should
    // not write an empty applyOps entry.
    if (stmts.empty())
        return;

    // We write the oplog entry in a side transaction so that we do not commit the now-prepared
    // transaction. We then return to the main transaction and set its 'prepareTimestamp'.
    repl::OpTime prepareOpTime;
    {
        Session::SideTransactionBlock sideTxn(opCtx);
        WriteUnitOfWork wuow(opCtx);
        prepareOpTime =
            logApplyOpsForTransaction(opCtx, session, stmts, true /* prepare */).writeOpTime;
        wuow.commit();
    }
    invariant(!prepareOpTime.isNull());
    opCtx->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
}

void OpObserverImpl::onTransactionAbort(OperationContext* opCtx) {
    invariant(opCtx->getTxnNumber());
}

void OpObserverImpl::onReplicationRollback(OperationContext* opCtx,
                                           const RollbackObserverInfo& rbInfo) {

    // Invalidate any in-memory auth data if necessary.
    const auto& rollbackNamespaces = rbInfo.rollbackNamespaces;
    if (rollbackNamespaces.count(AuthorizationManager::versionCollectionNamespace) == 1 ||
        rollbackNamespaces.count(AuthorizationManager::usersCollectionNamespace) == 1 ||
        rollbackNamespaces.count(AuthorizationManager::rolesCollectionNamespace) == 1) {
        AuthorizationManager::get(opCtx->getServiceContext())->invalidateUserCache();
    }

    // If there were ops rolled back that were part of operations on a session, then invalidate the
    // session cache.
    if (rbInfo.rollbackSessionIds.size() > 0) {
        SessionCatalog::get(opCtx)->invalidateSessions(opCtx, boost::none);
    }

    // Reset the key manager cache.
    auto validator = LogicalTimeValidator::get(opCtx);
    if (validator) {
        validator->resetKeyManagerCache();
    }

    // Check if the shard identity document rolled back.
    if (rbInfo.shardIdentityRolledBack) {
        fassertFailedNoTrace(50712);
    }
}

}  // namespace mongo

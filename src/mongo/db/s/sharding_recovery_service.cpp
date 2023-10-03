/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include <fmt/format.h>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_critical_section_document_gen.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/shard_authoritative_catalog_gen.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/write_concern.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_collection_gen.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace sharding_recovery_util {

bool inRecoveryMode(OperationContext* opCtx) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->getSettings().isReplSet()) {
        return false;
    }

    const auto memberState = replCoord->getMemberState();
    return memberState.startup2() || memberState.rollback();
}

}  // namespace sharding_recovery_util

namespace {
const StringData kShardingIndexCatalogEntriesFieldName = "indexes"_sd;
const auto serviceDecorator = ServiceContext::declareDecoration<ShardingRecoveryService>();

AggregateCommandRequest makeCollectionsAndIndexesAggregation(OperationContext* opCtx) {
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, nullptr, NamespaceString::kShardCollectionCatalogNamespace);
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[NamespaceString::kShardCollectionCatalogNamespace.coll()] = {
        NamespaceString::kShardCollectionCatalogNamespace, std::vector<BSONObj>()};
    resolvedNamespaces[NamespaceString::kShardIndexCatalogNamespace.coll()] = {
        NamespaceString::kShardIndexCatalogNamespace, std::vector<BSONObj>()};
    expCtx->setResolvedNamespaces(resolvedNamespaces);

    using Doc = Document;
    using Arr = std::vector<Value>;

    Pipeline::SourceContainer stages;

    // 1. Match all entries in config.shard.collections with indexVersion.
    // {
    //      $match: {
    //          indexVersion: {
    //              $exists: true
    //          }
    //      }
    // }
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{ShardAuthoritativeCollectionType::kIndexVersionFieldName, Doc{{"$exists", true}}}}
            .toBson(),
        expCtx));

    // 2. Retrieve config.shard.indexes entries with the same uuid as the one from the
    // config.shard.collections document.
    //
    // The $lookup stage gets the config.shard.indexes documents and puts them in a field called
    // "indexes" in the document produced during stage 1.
    //
    // {
    //      $lookup: {
    //          from: "shard.indexes",
    //          as: "indexes",
    //          localField: "uuid",
    //          foreignField: "collectionUUID"
    //      }
    // }
    const Doc lookupPipeline{{"from", NamespaceString::kShardIndexCatalogNamespace.coll()},
                             {"as", kShardingIndexCatalogEntriesFieldName},
                             {"localField", ShardAuthoritativeCollectionType::kUuidFieldName},
                             {"foreignField", IndexCatalogType::kCollectionUUIDFieldName}};

    stages.emplace_back(DocumentSourceLookUp::createFromBson(
        Doc{{"$lookup", lookupPipeline}}.toBson().firstElement(), expCtx));

    auto pipeline = Pipeline::create(std::move(stages), expCtx);
    auto serializedPipeline = pipeline->serializeToBson();
    return AggregateCommandRequest(NamespaceString::kShardCollectionCatalogNamespace,
                                   std::move(serializedPipeline));
}
}  // namespace

ShardingRecoveryService* ShardingRecoveryService::get(ServiceContext* serviceContext) {
    return &serviceDecorator(serviceContext);
}

ShardingRecoveryService* ShardingRecoveryService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

const ReplicaSetAwareServiceRegistry::Registerer<ShardingRecoveryService>
    shardingRecoveryServiceRegisterer("ShardingRecoveryService");

void ShardingRecoveryService::acquireRecoverableCriticalSectionBlockWrites(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& reason,
    const WriteConcernOptions& writeConcern) {
    LOGV2_DEBUG(5656600,
                3,
                "Acquiring recoverable critical section blocking writes",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

    tassert(7032360,
            fmt::format("Can't acquire recoverable critical section for collection '{}' with "
                        "reason '{}' while holding locks",
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            !opCtx->lockState()->isLocked());

    {
        Lock::GlobalLock lk(opCtx, MODE_IX);
        boost::optional<AutoGetDb> dbLock;
        boost::optional<AutoGetCollection> collLock;
        if (nsIsDbOnly(NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()))) {
            tassert(8096300,
                    "Cannot acquire critical section on the config database",
                    !nss.isConfigDB());
            dbLock.emplace(opCtx, nss.dbName(), MODE_S);
        } else {
            if (nss.isConfigDB()) {
                // Take the 'config' database lock in mode IX to prevent lock upgrade when we later
                // write to kCollectionCriticalSectionsNamespace.
                dbLock.emplace(opCtx, nss.dbName(), MODE_IX);
            }

            collLock.emplace(opCtx,
                             nss,
                             MODE_S,
                             AutoGetCollection::Options{}.viewMode(
                                 auto_get_collection::ViewMode::kViewsPermitted));
        }

        DBDirectClient dbClient(opCtx);
        FindCommandRequest findRequest{NamespaceString::kCollectionCriticalSectionsNamespace};
        findRequest.setFilter(
            BSON(CollectionCriticalSectionDocument::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())));
        auto cursor = dbClient.find(std::move(findRequest));

        // if there is a doc with the same nss -> in order to not fail it must have the same
        // reason
        if (cursor->more()) {
            const auto bsonObj = cursor->next();
            const auto collCSDoc = CollectionCriticalSectionDocument::parse(
                IDLParserContext("AcquireRecoverableCSBW"), bsonObj);

            tassert(7032368,
                    fmt::format("Trying to acquire a  critical section blocking writes for "
                                "namespace '{}' and reason '{}' but it is already taken by another "
                                "operation with different reason '{}'",
                                nss.toStringForErrorMsg(),
                                reason.toString(),
                                collCSDoc.getReason().toString()),
                    collCSDoc.getReason().woCompare(reason) == 0);

            LOGV2_DEBUG(5656601,
                        3,
                        "The recoverable critical section was already acquired to block "
                        "writes, do nothing",
                        logAttrs(nss),
                        "reason"_attr = reason,
                        "writeConcern"_attr = writeConcern);

            return;
        }

        // The collection critical section is not taken, try to acquire it.

        // The following code will try to add a doc to config.criticalCollectionSections:
        // - If everything goes well, the shard server op observer will acquire the in-memory
        // CS.
        // - Otherwise this call will fail and the CS won't be taken (neither persisted nor
        // in-mem)
        CollectionCriticalSectionDocument newDoc(nss, reason, false /* blockReads */);

        const auto commandResponse = dbClient.runCommand([&] {
            write_ops::InsertCommandRequest insertOp(
                NamespaceString::kCollectionCriticalSectionsNamespace);
            insertOp.setDocuments({newDoc.toBSON()});
            return insertOp.serialize({});
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        BatchedCommandResponse batchedResponse;
        std::string unusedErrmsg;
        batchedResponse.parseBSON(commandReply, &unusedErrmsg);
        tassert(7032369,
                fmt::format(
                    "Insert did not add any doc to collection '{}' for namespace '{}' "
                    "and reason '{}'",
                    nss.toStringForErrorMsg(),
                    reason.toString(),
                    NamespaceString::kCollectionCriticalSectionsNamespace.toStringForErrorMsg()),
                batchedResponse.getN() > 0);
    }

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));

    LOGV2_DEBUG(5656602,
                2,
                "Acquired recoverable critical section blocking writes",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);
}

void ShardingRecoveryService::promoteRecoverableCriticalSectionToBlockAlsoReads(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& reason,
    const WriteConcernOptions& writeConcern) {
    LOGV2_DEBUG(5656603,
                3,
                "Promoting recoverable critical section to also block reads",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

    tassert(7032364,
            fmt::format("Can't promote recoverable critical section for collection '{}' with "
                        "reason '{}' while holding locks",
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            !opCtx->lockState()->isLocked());

    {
        boost::optional<AutoGetDb> dbLock;
        boost::optional<AutoGetCollection> collLock;
        if (nsIsDbOnly(NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()))) {
            dbLock.emplace(opCtx, nss.dbName(), MODE_X);
        } else {
            collLock.emplace(opCtx,
                             nss,
                             MODE_X,
                             AutoGetCollection::Options{}.viewMode(
                                 auto_get_collection::ViewMode::kViewsPermitted));
        }

        DBDirectClient dbClient(opCtx);
        FindCommandRequest findRequest{NamespaceString::kCollectionCriticalSectionsNamespace};
        findRequest.setFilter(
            BSON(CollectionCriticalSectionDocument::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())));
        auto cursor = dbClient.find(std::move(findRequest));

        tassert(7032361,
                fmt::format(
                    "Trying to acquire a critical section blocking reads for namespace '{}' and "
                    "reason '{}' but the critical section wasn't acquired first blocking writers.",
                    nss.toStringForErrorMsg(),
                    reason.toString()),
                cursor->more());
        BSONObj bsonObj = cursor->next();
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            IDLParserContext("AcquireRecoverableCSBR"), bsonObj);

        tassert(7032362,
                fmt::format(
                    "Trying to acquire a critical section blocking reads for namespace '{}' and "
                    "reason "
                    "'{}' but it is already taken by another operation with different reason '{}'",
                    nss.toStringForErrorMsg(),
                    reason.toString(),
                    collCSDoc.getReason().toString()),
                collCSDoc.getReason().woCompare(reason) == 0);

        // if there is a document with the same nss, reason and blocking reads -> do nothing,
        // the CS is already taken!
        if (collCSDoc.getBlockReads()) {
            LOGV2_DEBUG(5656604,
                        3,
                        "The recoverable critical section was already promoted to also block "
                        "reads, do nothing",
                        logAttrs(nss),
                        "reason"_attr = reason,
                        "writeConcern"_attr = writeConcern);
            return;
        }

        // The CS is in the catch-up phase, try to advance it to the commit phase.

        // The following code will try to update a doc from config.criticalCollectionSections:
        // - If everything goes well, the shard server op observer will advance the in-memory CS
        // to the
        //   commit phase (blocking readers).
        // - Otherwise this call will fail and the CS won't be advanced (neither persisted nor
        // in-mem)
        auto commandResponse = dbClient.runCommand([&] {
            const auto query =
                BSON(CollectionCriticalSectionDocument::kNssFieldName
                     << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                     << CollectionCriticalSectionDocument::kReasonFieldName << reason);
            const auto update = BSON(
                "$set" << BSON(CollectionCriticalSectionDocument::kBlockReadsFieldName << true));

            write_ops::UpdateCommandRequest updateOp(
                NamespaceString::kCollectionCriticalSectionsNamespace);
            auto updateModification = write_ops::UpdateModification::parseFromClassicUpdate(update);
            write_ops::UpdateOpEntry updateEntry(query, updateModification);
            updateOp.setUpdates({updateEntry});

            return updateOp.serialize({});
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        BatchedCommandResponse batchedResponse;
        std::string unusedErrmsg;
        batchedResponse.parseBSON(commandReply, &unusedErrmsg);
        tassert(
            7032363,
            fmt::format("Update did not modify any doc from collection '{}' for namespace '{}' "
                        "and reason '{}'",
                        NamespaceString::kCollectionCriticalSectionsNamespace.toStringForErrorMsg(),
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            batchedResponse.getNModified() > 0);
    }

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));

    LOGV2_DEBUG(5656605,
                2,
                "Promoted recoverable critical section to also block reads",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);
}

void ShardingRecoveryService::releaseRecoverableCriticalSection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& reason,
    const WriteConcernOptions& writeConcern,
    bool throwIfReasonDiffers) {
    LOGV2_DEBUG(5656606,
                3,
                "Releasing recoverable critical section",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);

    tassert(7032365,
            fmt::format("Can't release recoverable critical section for collection '{}' with "
                        "reason '{}' while holding locks",
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            !opCtx->lockState()->isLocked());

    {
        boost::optional<AutoGetDb> dbLock;
        boost::optional<AutoGetCollection> collLock;
        if (nsIsDbOnly(NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()))) {
            dbLock.emplace(opCtx, nss.dbName(), MODE_X);
        } else {
            collLock.emplace(opCtx,
                             nss,
                             MODE_X,
                             AutoGetCollection::Options{}.viewMode(
                                 auto_get_collection::ViewMode::kViewsPermitted));
        }

        DBDirectClient dbClient(opCtx);

        const auto queryNss =
            BSON(CollectionCriticalSectionDocument::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        FindCommandRequest findRequest{NamespaceString::kCollectionCriticalSectionsNamespace};
        findRequest.setFilter(queryNss);
        auto cursor = dbClient.find(std::move(findRequest));

        // if there is no document with the same nss -> do nothing!
        if (!cursor->more()) {
            LOGV2_DEBUG(5656607,
                        3,
                        "The recoverable critical section was already released, do nothing",
                        logAttrs(nss),
                        "reason"_attr = reason,
                        "writeConcern"_attr = writeConcern);
            return;
        }

        BSONObj bsonObj = cursor->next();
        const auto collCSDoc = CollectionCriticalSectionDocument::parse(
            IDLParserContext("ReleaseRecoverableCS"), bsonObj);

        const bool isDifferentReason = collCSDoc.getReason().woCompare(reason) != 0;
        if (MONGO_unlikely(!throwIfReasonDiffers && isDifferentReason)) {
            LOGV2_DEBUG(7019701,
                        2,
                        "Impossible to release recoverable critical section since it was taken by "
                        "another operation with different reason",
                        logAttrs(nss),
                        "callerReason"_attr = reason,
                        "storedReason"_attr = collCSDoc.getReason(),
                        "writeConcern"_attr = writeConcern);
            return;
        }

        tassert(7032366,
                fmt::format("Trying to release a critical for namespace '{}' and reason '{}' but "
                            "it is already taken by another operation with different reason '{}'",
                            nss.toStringForErrorMsg(),
                            reason.toString(),
                            collCSDoc.getReason().toString()),
                !isDifferentReason);


        // The collection critical section is taken (in any phase), try to release it.

        // The following code will try to remove a doc from config.criticalCollectionSections:
        // - If everything goes well, the shard server op observer will release the in-memory CS
        // - Otherwise this call will fail and the CS won't be released (neither persisted nor
        // in-mem)

        auto commandResponse = dbClient.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(
                NamespaceString::kCollectionCriticalSectionsNamespace);

            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(queryNss);
                entry.setMulti(true);
                return entry;
            }()});

            return deleteOp.serialize({});
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        BatchedCommandResponse batchedResponse;
        std::string unusedErrmsg;
        batchedResponse.parseBSON(commandReply, &unusedErrmsg);
        tassert(
            7032367,
            fmt::format("Delete did not remove any doc from collection '{}' for namespace '{}' "
                        "and reason '{}'",
                        NamespaceString::kCollectionCriticalSectionsNamespace.toStringForErrorMsg(),
                        nss.toStringForErrorMsg(),
                        reason.toString()),
            batchedResponse.getN() > 0);
    }

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));

    LOGV2_DEBUG(5656608,
                2,
                "Released recoverable critical section",
                logAttrs(nss),
                "reason"_attr = reason,
                "writeConcern"_attr = writeConcern);
}

void ShardingRecoveryService::recoverRecoverableCriticalSections(OperationContext* opCtx) {
    LOGV2_DEBUG(5604000, 2, "Recovering all recoverable critical sections");

    // Release all in-memory critical sections
    for (const auto& nss : CollectionShardingState::getCollectionNames(opCtx)) {
        AutoGetCollection collLock(
            opCtx,
            nss,
            MODE_X,
            AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));
        auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss);
        scopedCsr->exitCriticalSectionNoChecks();
    }
    for (const auto& dbName : DatabaseShardingState::getDatabaseNames(opCtx)) {
        AutoGetDb dbLock(opCtx, dbName, MODE_X);
        auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, dbName);
        scopedDss->exitCriticalSectionNoChecks(opCtx);
    }

    // Map the critical sections that are on disk to memory
    PersistentTaskStore<CollectionCriticalSectionDocument> store(
        NamespaceString::kCollectionCriticalSectionsNamespace);
    store.forEach(opCtx, BSONObj{}, [&opCtx](const CollectionCriticalSectionDocument& doc) {
        const auto& nss = doc.getNss();
        {
            if (nsIsDbOnly(
                    NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()))) {
                AutoGetDb dbLock(opCtx, nss.dbName(), MODE_X);
                auto scopedDss =
                    DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, nss.dbName());
                scopedDss->enterCriticalSectionCatchUpPhase(opCtx, doc.getReason());
                if (doc.getBlockReads()) {
                    scopedDss->enterCriticalSectionCommitPhase(opCtx, doc.getReason());
                }
            } else {
                AutoGetCollection collLock(opCtx,
                                           nss,
                                           MODE_X,
                                           AutoGetCollection::Options{}.viewMode(
                                               auto_get_collection::ViewMode::kViewsPermitted));
                auto scopedCsr =
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx,
                                                                                         nss);
                scopedCsr->enterCriticalSectionCatchUpPhase(doc.getReason());
                if (doc.getBlockReads()) {
                    scopedCsr->enterCriticalSectionCommitPhase(doc.getReason());
                }
            }

            return true;
        }
    });

    LOGV2_DEBUG(5604001, 2, "Recovered all recoverable critical sections");
}

void ShardingRecoveryService::recoverStates(OperationContext* opCtx,
                                            const std::set<NamespaceString>& rollbackNamespaces) {

    if (rollbackNamespaces.find(NamespaceString::kCollectionCriticalSectionsNamespace) !=
        rollbackNamespaces.end()) {
        ShardingRecoveryService::get(opCtx)->recoverRecoverableCriticalSections(opCtx);
    }

    if (rollbackNamespaces.find(NamespaceString::kShardCollectionCatalogNamespace) !=
            rollbackNamespaces.end() ||
        rollbackNamespaces.find(CollectionType::ConfigNS) != rollbackNamespaces.end()) {
        ShardingRecoveryService::get(opCtx)->recoverIndexesCatalog(opCtx);
    }
}

void ShardingRecoveryService::onInitialDataAvailable(OperationContext* opCtx,
                                                     bool isMajorityDataAvailable) {
    recoverRecoverableCriticalSections(opCtx);
    recoverIndexesCatalog(opCtx);
}

void ShardingRecoveryService::recoverIndexesCatalog(OperationContext* opCtx) {
    LOGV2_DEBUG(6686500, 2, "Recovering all sharding index catalog");

    // Reset all in-memory index versions.
    const auto collectionNames = CollectionShardingState::getCollectionNames(opCtx);
    for (const auto& collName : collectionNames) {
        try {
            AutoGetCollection collLock(opCtx, collName, MODE_X);
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, collName)
                ->clearIndexes(opCtx);
        } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
            LOGV2_DEBUG(6686501,
                        2,
                        "Skipping attempting to clear indexes for a view in "
                        "recoverIndexCatalogs",
                        logAttrs(collName));
        }
    }
    DBDirectClient client(opCtx);
    auto aggRequest = makeCollectionsAndIndexesAggregation(opCtx);

    auto cursor = uassertStatusOKWithContext(
        DBClientCursor::fromAggregationRequest(
            &client, aggRequest, true /* secondaryOk */, true /* useExhaust */),
        "Failed to establish a cursor for aggregation");


    while (cursor->more()) {
        auto doc = cursor->nextSafe();
        const auto nss =
            NamespaceStringUtil::deserialize(boost::none,
                                             doc[CollectionType::kNssFieldName].String(),
                                             SerializationContext::stateDefault());
        auto indexVersion = doc[CollectionType::kIndexVersionFieldName].timestamp();
        for (const auto& idx : doc[kShardingIndexCatalogEntriesFieldName].Array()) {
            auto indexEntry = IndexCatalogType::parse(
                IDLParserContext("recoverIndexesCatalogContext"), idx.Obj());
            AutoGetCollection collLock(opCtx, nss, MODE_X);
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx,
                                                                                 collLock->ns())
                ->addIndex(opCtx, indexEntry, {indexEntry.getCollectionUUID(), indexVersion});
        }
    }
    LOGV2_DEBUG(6686502, 2, "Recovered all index versions");
}

}  // namespace mongo

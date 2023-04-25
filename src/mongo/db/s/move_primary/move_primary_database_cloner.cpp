/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/move_primary/move_primary_database_cloner.h"

#include "mongo/base/string_data.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_list_catalog.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/database_cloner_gen.h"
#include "mongo/db/s/move_primary/move_primary_collection_cloner.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kMovePrimary

namespace mongo {

MovePrimaryDatabaseCloner::MovePrimaryDatabaseCloner(
    const DatabaseName& dbName,
    const stdx::unordered_set<NamespaceString>& shardedColls,
    const Timestamp& startCloningOpTime,
    MovePrimarySharedData* sharedData,
    const HostAndPort& source,
    DBClientConnection* client,
    repl::StorageInterface* storageInterface,
    ThreadPool* dbPool,
    ShardingCatalogClient* catalogClient)
    : MovePrimaryBaseCloner(
          "MovePrimaryDatabaseCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _dbName(dbName),
      _shardedColls(shardedColls),
      _startCloningOpTime(startCloningOpTime),
      _listExistingCollectionsOnDonorStage(
          "listCollections", this, &MovePrimaryDatabaseCloner::listExistingCollectionsOnDonorStage),
      _listExistingCollectionsOnRecipientStage(
          "listExistingCollections",
          this,
          &MovePrimaryDatabaseCloner::listExistingCollectionsOnRecipientStage),
      _catalogClient(catalogClient) {
    invariant(!_dbName.db().empty());
    _opCtxHolder = cc().makeOperationContext();
    if (!_catalogClient) {
        _catalogClient = Grid::get(_opCtxHolder.get())->catalogClient();
    }
}

repl::BaseCloner::ClonerStages MovePrimaryDatabaseCloner::getStages() {
    return {&_listExistingCollectionsOnDonorStage, &_listExistingCollectionsOnRecipientStage};
}

void MovePrimaryDatabaseCloner::preStage() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.start = getSharedData()->getClock()->now();
}

void MovePrimaryDatabaseCloner::calculateListCatalogEntriesForDonor() {
    auto opCtx = _opCtxHolder.get();
    auto nss = NamespaceString(DatabaseName::kAdmin.db());
    boost::intrusive_ptr<ExpressionContext> expCtx =
        make_intrusive<ExpressionContext>(opCtx, nullptr, nss);

    Pipeline::SourceContainer stages;
    stages.emplace_back(DocumentSourceListCatalog::createFromBson(
        BSON("$listCatalog" << BSONObj()).firstElement(), expCtx));
    stages.emplace_back(
        DocumentSourceMatch::create(BSON("db" << BSON("$eq" << _dbName.toString())), expCtx));
    stages.emplace_back(DocumentSourceMatch::create(BSON("type" << BSON("$eq"
                                                                        << "collection")),
                                                    expCtx));
    const auto serializedPipeline = Pipeline::create(std::move(stages), expCtx)->serializeToBson();

    AggregateCommandRequest aggRequest(nss, std::move(serializedPipeline));
    auto readConcern = repl::ReadConcernArgs(
        boost::optional<LogicalTime>(_startCloningOpTime),
        boost::optional<repl::ReadConcernLevel>(repl::ReadConcernLevel::kMajorityReadConcern));
    aggRequest.setReadConcern(readConcern.toBSONInner());
    aggRequest.setWriteConcern(WriteConcernOptions());

    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        getClient(), std::move(aggRequest), true /* secondaryOk */, false /* useExhaust*/));

    while (cursor->more()) {
        auto entry = cursor->next();
        CollectionParams coll =
            MovePrimaryBaseCloner::CollectionParams::parseCollectionParamsFromBSON(entry);
        coll.shardedColl = _shardedColls.contains(coll.ns);
        _donorCollections.emplace_back(coll);
    }
}

void MovePrimaryDatabaseCloner::calculateListCatalogEntriesForRecipient() {

    auto opCtx = _opCtxHolder.get();
    auto recipientShardedColls = _catalogClient->getAllShardedCollectionsForDb(
        opCtx, _dbName.toString(), repl::ReadConcernLevel::kMajorityReadConcern);

    stdx::unordered_set<NamespaceString> shardedCollsSet;
    std::copy(recipientShardedColls.begin(),
              recipientShardedColls.end(),
              inserter(shardedCollsSet, shardedCollsSet.end()));

    AutoGetDbForReadMaybeLockFree lockFreeReadBlock(opCtx, _dbName);
    auto parseCollection = [&](const Collection* coll) {
        invariant(coll);
        bool shardedColl = shardedCollsSet.contains(coll->ns());
        _recipientCollections.emplace_back(CollectionParams{coll->ns(), coll->uuid(), shardedColl});
        return true;
    };

    if (opCtx->isLockFreeReadsOp()) {
        auto collectionCatalog = CollectionCatalog::get(opCtx);
        for (auto it = collectionCatalog->begin(opCtx, _dbName);
             it != collectionCatalog->end(opCtx);
             ++it) {
            parseCollection(*it);
        }
    } else {
        mongo::catalog::forEachCollectionFromDb(opCtx, _dbName, MODE_IS, parseCollection);
    }
}

repl::BaseCloner::AfterStageBehavior
MovePrimaryDatabaseCloner::listExistingCollectionsOnDonorStage() {
    calculateListCatalogEntriesForDonor();
    // Verify donor collections
    stdx::unordered_set<NamespaceString> seen;
    for (auto& info : _donorCollections) {
        auto collectionNamespace = info.ns;
        if (collectionNamespace.isSystem() && !collectionNamespace.isReplicated()) {
            continue;
        }
        LOGV2_DEBUG(7307501,
                    2,
                    "Allowing cloning of collection",
                    "namespace"_attr = collectionNamespace.ns());

        bool canInsert = seen.insert(collectionNamespace).second;
        uassert(7307502,
                str::stream() << "Donor collection list contains duplicate collection name "
                              << "'" << collectionNamespace.ns(),
                canInsert);
    }
    return kContinueNormally;
}

repl::BaseCloner::AfterStageBehavior
MovePrimaryDatabaseCloner::listExistingCollectionsOnRecipientStage() {
    calculateListCatalogEntriesForRecipient();

    std::sort(_donorCollections.begin(), _donorCollections.end());
    std::sort(_recipientCollections.begin(), _recipientCollections.end());

    boost::optional<CollectionParams> lastUnshardedColl;

    for (auto& info : _recipientCollections) {
        // Ensure no unsharded collection exists on the recipient if
        // the operation is not being resumed.
        uassert(7307503,
                str::stream() << "Unsharded collection " << info.ns.ns()
                              << " exists prior to data cloning on recipient",
                (info.shardedColl || getSharedData()->getResumePhase() == ResumePhase::kDataSync));

        // Verify that the recipient collection exists on the donor with the same Namespace & UUID.
        auto donorColl = std::lower_bound(_donorCollections.begin(), _donorCollections.end(), info);
        uassert(7307504,
                str::stream() << "Collection " << info.ns.ns() << " with UUID " << info.uuid
                              << " exists on recipient but not on donor",
                ((donorColl != _donorCollections.end()) && (donorColl->uuid == info.uuid) &&
                 (donorColl->ns == info.ns)));

        if (!info.shardedColl) {
            lastUnshardedColl = info;
        }
    }

    set_difference(_donorCollections.begin(),
                   _donorCollections.end(),
                   _recipientCollections.begin(),
                   _recipientCollections.end(),
                   std::back_inserter(_collectionsToClone));

    // Record the last unsharded collection (exists only during a resume) to check if it was only
    // partially cloned. This check will be performed by the MovePrimaryCollectionCloner
    if (lastUnshardedColl.has_value()) {
        _collectionsToClone.push_front(*lastUnshardedColl);
    }
    if (!_collectionsToClone.empty()) {
        auto collectionNs = _collectionsToClone.front().ns;
        LOGV2(7307505,
              "MovePrimaryDatabaseCloner resumes cloning",
              "collNs"_attr = collectionNs,
              "migrationId"_attr = getSharedData()->getMigrationId(),
              "resumeFrom"_attr = _donorCollections.front().ns);
    } else {
        LOGV2(7307506,
              "MovePrimaryDatabaseCloner has already cloned all collections",
              "migrationId"_attr = getSharedData()->getMigrationId(),
              "dbName"_attr = _dbName);
    }
    return kContinueNormally;
}

void MovePrimaryDatabaseCloner::postStage() {
    for (const auto& coll : _collectionsToClone) {
        std::unique_ptr<MovePrimaryCollectionCloner> currentCollectionCloner;
        {
            stdx::lock_guard<Latch> lk(_mutex);
            currentCollectionCloner =
                std::make_unique<MovePrimaryCollectionCloner>(coll,
                                                              getSharedData(),
                                                              getSource(),
                                                              getClient(),
                                                              getStorageInterface(),
                                                              getDBPool());
        }
        auto collStatus = currentCollectionCloner->run();
        {
            stdx::lock_guard<Latch> lk(_mutex);
            currentCollectionCloner = nullptr;
        }
        if (collStatus.isOK()) {
            LOGV2_DEBUG(7307507,
                        1,
                        "MovePrimaryCollectionCloner finished",
                        "namespace"_attr = coll.ns,
                        "migrationId"_attr = getSharedData()->getMigrationId());
        } else {
            LOGV2_ERROR(7307508,
                        "MovePrimaryCollectionCloner failed",
                        "namespace"_attr = coll.ns,
                        "migrationId"_attr = getSharedData()->getMigrationId(),
                        "error"_attr = collStatus.toString());
            auto message = collStatus.withContext(str::stream() << "Error cloning collection '"
                                                                << coll.ns.toString() << "'");
            // Abort the MovePrimaryDatabaseCloner if the collection clone failed.
            setSyncFailedStatus(collStatus.withReason(message.toString()));
            return;
        }
    }
}

}  // namespace mongo

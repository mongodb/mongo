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

#include "mongo/s/catalog/sharding_catalog_client_impl.h"

#include <fmt/format.h>
#include <iomanip>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_remote_gen.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/pcre.h"
#include "mongo/util/pcre_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using namespace fmt::literals;

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const ReadPreferenceSetting kConfigPrimaryPreferredSelector(ReadPreference::PrimaryPreferred,
                                                            TagSet{});
const int kMaxReadRetry = 3;
const int kMaxWriteRetry = 3;

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setStatus(status);
}

AggregateCommandRequest makeCollectionAndChunksAggregation(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const ChunkVersion& sinceVersion) {
    auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, CollectionType::ConfigNS);
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[CollectionType::ConfigNS.coll()] = {CollectionType::ConfigNS,
                                                           std::vector<BSONObj>()};
    resolvedNamespaces[ChunkType::ConfigNS.coll()] = {ChunkType::ConfigNS, std::vector<BSONObj>()};
    expCtx->setResolvedNamespaces(resolvedNamespaces);

    using Doc = Document;
    using Arr = std::vector<Value>;

    Pipeline::SourceContainer stages;

    // 1. Match config.collections entries with {_id: nss}. This stage will produce, at most, one
    // config.collections document.
    // {
    //     $match: {
    //         _id: <nss>
    //     }
    // }
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{CollectionType::kNssFieldName, nss.toString()}}.toBson(), expCtx));

    // 2. Two $unionWith stages guarded by a mutually exclusive condition on whether the refresh is
    // incremental ('lastmodEpoch' matches sinceVersion.epoch), so that only a single one of them
    // will possibly execute their $lookup stage. This is necessary because the query optimizer is
    // not able to use indexes when a $match inside a $lookup includes a $cond operator.
    //
    // The $lookup stages get the config.chunks documents according to the
    // type of refresh (incremental or full), sorted by ascending 'lastmod'. The $lookup is
    // immediately followed by $unwind to take advantage of the $lookup + $unwind coalescence
    // optimization which avoids creating large intermediate documents.
    //
    // This $unionWith stage will produce one result document for each config.chunks document
    // matching the refresh query.
    // Note that we must not make any assumption on where the document produced by stage 1 will be
    // placed in the response in relation with the documents produced by stage 2. The
    // config.collections document produced in stage 1 could be interleaved between the
    // config.chunks documents produced by stage 2.
    //
    // {
    //     $unionWith: {
    //         coll: "collections",
    //         pipeline: [
    //             { $match: { _id: <nss> } },
    //             { $match: { lastmodEpoch: <sinceVersion.epoch> } },
    //             {
    //                 $lookup: {
    //                     from: "chunks",
    //                     as: "chunks",
    //                     let: { local_uuid: "$uuid" },
    //                     pipeline: [
    //                         {
    //                             $match: {
    //                                 $expr: {
    //                                     $eq: ["$uuid", "$$local_uuid"],
    //                                 },
    //                             }
    //                         },
    //                         { $match: { lastmod: { $gte: <sinceVersion> } } },
    //                         {
    //                             $sort: {
    //                                 lastmod: 1
    //                             }
    //                         }
    //                     ]
    //                 }
    //             },
    //             {
    //                 $unwind: {
    //                     path: "$chunks"
    //                 }
    //             },
    //             {
    //                 $project: { _id: false, chunks: true }
    //             },
    //         ]
    //     }
    // },
    // {
    //     $unionWith: {
    //         coll: "collections",
    //         pipeline: [
    //             { $match: { _id: <nss> } },
    //             { $match: { lastmodEpoch: { $ne: <sinceVersion.epoch> } } },
    //             {
    //                 $lookup: {
    //                     from: "chunks",
    //                     as: "chunks",
    //                     let: { local_uuid: "$uuid" },
    //                     pipeline: [
    //                         {
    //                             $match: {
    //                                 $expr: {
    //                                     $eq: ["$uuid", "$$local_uuid"],
    //                                 },
    //                             }
    //                         },
    //                         {
    //                             $sort: {
    //                                 lastmod: 1
    //                             }
    //                         }
    //                     ]
    //                 }
    //             },
    //             {
    //                 $unwind: {
    //                     path: "$chunks"
    //                 }
    //             },
    //             {
    //                 $project: { _id: false, chunks: true }
    //             },
    //         ]
    //     }
    // }
    const auto buildUnionWithFn = [&](bool incremental) {
        const auto lastmodEpochMatch = Doc{{incremental ? "$eq" : "$ne", sinceVersion.epoch()}};

        const auto letExpr = Doc{{"local_uuid", "$" + CollectionType::kUuidFieldName}};

        const auto uuidExpr =
            Arr{Value{"$" + ChunkType::collectionUUID.name()}, Value{"$$local_uuid"_sd}};

        constexpr auto chunksLookupOutputFieldName = "chunks"_sd;

        const auto lookupPipeline = [&]() {
            return Doc{
                {"from", ChunkType::ConfigNS.coll()},
                {"as", chunksLookupOutputFieldName},
                {"let", letExpr},
                {"pipeline",
                 Arr{Value{Doc{{"$match", Doc{{"$expr", Doc{{"$eq", uuidExpr}}}}}}},
                     incremental
                         ? Value{Doc{{"$match",
                                      Doc{{ChunkType::lastmod.name(),
                                           Doc{{"$gte", Timestamp(sinceVersion.toLong())}}}}}}}
                         : Value{/*noop*/},
                     Value{Doc{{"$sort", Doc{{ChunkType::lastmod.name(), 1}}}}}}}};
        }();

        return Doc{
            {"coll", CollectionType::ConfigNS.coll()},
            {"pipeline",
             Arr{Value{Doc{{"$match", Doc{{CollectionType::kNssFieldName, nss.toString()}}}}},
                 Value{Doc{{"$match", Doc{{CollectionType::kEpochFieldName, lastmodEpochMatch}}}}},
                 Value{Doc{{"$lookup", lookupPipeline}}},
                 Value{Doc{{"$unwind", Doc{{"path", "$" + chunksLookupOutputFieldName}}}}},
                 Value{Doc{
                     {"$project", Doc{{"_id", false}, {chunksLookupOutputFieldName, true}}}}}}}};
    };

    stages.emplace_back(DocumentSourceUnionWith::createFromBson(
        Doc{{"$unionWith", buildUnionWithFn(true /* incremental */)}}.toBson().firstElement(),
        expCtx));

    stages.emplace_back(DocumentSourceUnionWith::createFromBson(
        Doc{{"$unionWith", buildUnionWithFn(false /* incremental */)}}.toBson().firstElement(),
        expCtx));

    auto pipeline = Pipeline::create(std::move(stages), expCtx);
    auto serializedPipeline = pipeline->serializeToBson();
    return AggregateCommandRequest(CollectionType::ConfigNS, std::move(serializedPipeline));
}

AggregateCommandRequest makeCollectionAndIndexesAggregation(OperationContext* opCtx,
                                                            const NamespaceString& nss) {
    auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, CollectionType::ConfigNS);
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[CollectionType::ConfigNS.coll()] = {CollectionType::ConfigNS,
                                                           std::vector<BSONObj>()};
    resolvedNamespaces[NamespaceString::kConfigsvrIndexCatalogNamespace.coll()] = {
        NamespaceString::kConfigsvrIndexCatalogNamespace, std::vector<BSONObj>()};
    expCtx->setResolvedNamespaces(resolvedNamespaces);

    using Doc = Document;
    using Arr = std::vector<Value>;

    Pipeline::SourceContainer stages;

    // 1. Match config.collections entries with {_id: nss}. This stage will produce, at most, one
    // config.collections document.
    // {
    //     $match: {
    //         _id: <nss>
    //     }
    // }
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{CollectionType::kNssFieldName, nss.toString()}}.toBson(), expCtx));

    // 2. Retrieve config.csrs.indexes entries with the same uuid as the one from the
    // config.collections document.
    //
    // The $lookup stage gets the config.csrs.indexes documents and puts them in a field called
    // "indexes" in the document produced during stage 1.
    //
    // {
    //      $lookup: {
    //          from: "csrs.indexes",
    //          as: "indexes",
    //          localField: "uuid",
    //          foreignField: "collectionUUID"
    //      }
    // }
    const Doc lookupPipeline{{"from", NamespaceString::kConfigsvrIndexCatalogNamespace.coll()},
                             {"as", "indexes"_sd},
                             {"localField", CollectionType::kUuidFieldName},
                             {"foreignField", IndexCatalogType::kCollectionUUIDFieldName}};

    stages.emplace_back(DocumentSourceLookUp::createFromBson(
        Doc{{"$lookup", lookupPipeline}}.toBson().firstElement(), expCtx));

    auto pipeline = Pipeline::create(std::move(stages), expCtx);
    auto serializedPipeline = pipeline->serializeToBson();
    return AggregateCommandRequest(CollectionType::ConfigNS, std::move(serializedPipeline));
}

}  // namespace

ShardingCatalogClientImpl::ShardingCatalogClientImpl(std::shared_ptr<Shard> overrideConfigShard)
    : _overrideConfigShard(std::move(overrideConfigShard)) {}

ShardingCatalogClientImpl::~ShardingCatalogClientImpl() = default;

DatabaseType ShardingCatalogClientImpl::getDatabase(OperationContext* opCtx,
                                                    StringData dbName,
                                                    repl::ReadConcernLevel readConcernLevel) {
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << dbName << " is not a valid db name",
            NamespaceString::validDBName(dbName, NamespaceString::DollarInDbNameBehavior::Allow));

    // The admin database is always hosted on the config server.
    if (dbName == DatabaseName::kAdmin.db()) {
        return DatabaseType(
            dbName.toString(), ShardId::kConfigServerId, DatabaseVersion::makeFixed());
    }

    // The config database's primary shard is always config, and it is always sharded.
    if (dbName == DatabaseName::kConfig.db()) {
        return DatabaseType(
            dbName.toString(), ShardId::kConfigServerId, DatabaseVersion::makeFixed());
    }

    auto result =
        _fetchDatabaseMetadata(opCtx, dbName.toString(), kConfigReadSelector, readConcernLevel);
    if (result == ErrorCodes::NamespaceNotFound) {
        // If we failed to find the database metadata on the 'nearest' config server, try again
        // against the primary, in case the database was recently created.
        return uassertStatusOK(
                   _fetchDatabaseMetadata(opCtx,
                                          dbName.toString(),
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          readConcernLevel))
            .value;
    }

    return uassertStatusOK(std::move(result)).value;
}

std::vector<DatabaseType> ShardingCatalogClientImpl::getAllDBs(OperationContext* opCtx,
                                                               repl::ReadConcernLevel readConcern) {
    auto dbs = uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                       kConfigReadSelector,
                                                       readConcern,
                                                       NamespaceString::kConfigDatabasesNamespace,
                                                       BSONObj(),
                                                       BSONObj(),
                                                       boost::none))
                   .value;

    std::vector<DatabaseType> databases;
    databases.reserve(dbs.size());
    for (const BSONObj& doc : dbs) {
        databases.emplace_back(DatabaseType::parse(IDLParserContext("DatabaseType"), doc));
    }

    return databases;
}

StatusWith<repl::OpTimeWith<DatabaseType>> ShardingCatalogClientImpl::_fetchDatabaseMetadata(
    OperationContext* opCtx,
    const std::string& dbName,
    const ReadPreferenceSetting& readPref,
    repl::ReadConcernLevel readConcernLevel) {
    invariant(dbName != DatabaseName::kAdmin.db() && dbName != DatabaseName::kConfig.db());

    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              readPref,
                                              readConcernLevel,
                                              NamespaceString::kConfigDatabasesNamespace,
                                              BSON(DatabaseType::kNameFieldName << dbName),
                                              BSONObj(),
                                              boost::none);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docsWithOpTime = findStatus.getValue();
    if (docsWithOpTime.value.empty()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName << " not found"};
    }

    invariant(docsWithOpTime.value.size() == 1);

    try {
        auto db =
            DatabaseType::parse(IDLParserContext("DatabaseType"), docsWithOpTime.value.front());
        return repl::OpTimeWith<DatabaseType>(db, docsWithOpTime.opTime);
    } catch (const DBException& e) {
        return e.toStatus("Failed to parse DatabaseType");
    }
}

HistoricalPlacement ShardingCatalogClientImpl::_fetchPlacementMetadata(
    OperationContext* opCtx, ConfigsvrGetHistoricalPlacement&& request) {
    auto remoteResponse = uassertStatusOK(_getConfigShard(opCtx)->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        DatabaseName::kAdmin.toString(),
        request.toBSON(BSONObj()),
        Shard::kDefaultConfigCommandTimeout,
        Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    uassertStatusOK(remoteResponse.commandStatus);

    auto placementDetails = ConfigsvrGetHistoricalPlacementResponse::parse(
        IDLParserContext("ShardingCatalogClient"), remoteResponse.response);

    return placementDetails.getHistoricalPlacement();
}

std::vector<BSONObj> ShardingCatalogClientImpl::runCatalogAggregation(
    OperationContext* opCtx,
    AggregateCommandRequest& aggRequest,
    const repl::ReadConcernArgs& readConcern,
    const Milliseconds& maxTimeout) {
    // Reads on the config server may run on any node in its replica set. Such reads use the config
    // time as an afterClusterTime token, but config time is only inclusive of majority committed
    // data, so we should not use a weaker read concern. Note if the local node is a config server,
    // it can use these concerns safely with a ShardLocal, which would require relaxing this
    // invariant.
    invariant(readConcern.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern ||
                  readConcern.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern ||
                  readConcern.getLevel() == repl::ReadConcernLevel::kLinearizableReadConcern,
              str::stream() << "Disallowed read concern: " << readConcern.toBSONInner());

    aggRequest.setReadConcern(readConcern.toBSONInner());
    aggRequest.setWriteConcern(WriteConcernOptions());

    const auto readPref = [&]() -> ReadPreferenceSetting {
        // (Ignore FCV check): This is in mongos so we expect to ignore FCV.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
            !gFeatureFlagCatalogShard.isEnabledAndIgnoreFCVUnsafe()) {
            // When the feature flag is on, the config server may read from any node in its replica
            // set, so we should use the typical config server read preference.
            return {};
        }

        const auto vcTime = VectorClock::get(opCtx)->getTime();
        ReadPreferenceSetting readPref{kConfigReadSelector};
        readPref.minClusterTime = vcTime.configTime().asTimestamp();
        return readPref;
    }();

    aggRequest.setUnwrappedReadPref(readPref.toContainingBSON());

    if (!serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        // Don't use a timeout on the config server to guarantee it can always refresh.
        const Milliseconds maxTimeMS = std::min(opCtx->getRemainingMaxTimeMillis(), maxTimeout);
        aggRequest.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));
    }

    // Run the aggregation
    std::vector<BSONObj> aggResult;
    auto callback = [&aggResult](const std::vector<BSONObj>& batch,
                                 const boost::optional<BSONObj>& postBatchResumeToken) {
        aggResult.insert(aggResult.end(),
                         std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
        return true;
    };

    const auto configShard = _getConfigShard(opCtx);
    for (int retry = 1; retry <= kMaxWriteRetry; retry++) {
        const Status status = configShard->runAggregation(opCtx, aggRequest, callback);
        if (retry < kMaxWriteRetry &&
            configShard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent)) {
            aggResult.clear();
            continue;
        }
        uassertStatusOK(status);
        break;
    }

    return aggResult;
}

CollectionType ShardingCatalogClientImpl::getCollection(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        repl::ReadConcernLevel readConcernLevel) {
    auto collDoc =
        uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                kConfigReadSelector,
                                                readConcernLevel,
                                                CollectionType::ConfigNS,
                                                BSON(CollectionType::kNssFieldName << nss.ns()),
                                                BSONObj(),
                                                1))
            .value;
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "collection " << nss.ns() << " not found",
            !collDoc.empty());

    return CollectionType(collDoc[0]);
}

CollectionType ShardingCatalogClientImpl::getCollection(OperationContext* opCtx,
                                                        const UUID& uuid,
                                                        repl::ReadConcernLevel readConcernLevel) {
    auto collDoc =
        uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                kConfigReadSelector,
                                                readConcernLevel,
                                                CollectionType::ConfigNS,
                                                BSON(CollectionType::kUuidFieldName << uuid),
                                                BSONObj(),
                                                1))
            .value;
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection with UUID '" << uuid << "' not found",
            !collDoc.empty());

    return CollectionType(collDoc[0]);
}

std::vector<CollectionType> ShardingCatalogClientImpl::getCollections(
    OperationContext* opCtx,
    StringData dbName,
    repl::ReadConcernLevel readConcernLevel,
    const BSONObj& sort) {
    BSONObjBuilder b;
    if (!dbName.empty())
        b.appendRegex(CollectionType::kNssFieldName, "^{}\\."_format(pcre_util::quoteMeta(dbName)));

    auto collDocs = uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                            kConfigReadSelector,
                                                            readConcernLevel,
                                                            CollectionType::ConfigNS,
                                                            b.obj(),
                                                            sort,
                                                            boost::none))
                        .value;
    std::vector<CollectionType> collections;
    collections.reserve(collDocs.size());
    for (const BSONObj& obj : collDocs)
        collections.emplace_back(obj);

    return collections;
}

std::vector<NamespaceString> ShardingCatalogClientImpl::getAllShardedCollectionsForDb(
    OperationContext* opCtx,
    StringData dbName,
    repl::ReadConcernLevel readConcern,
    const BSONObj& sort) {
    auto collectionsOnConfig = getCollections(opCtx, dbName, readConcern, sort);

    std::vector<NamespaceString> collectionsToReturn;
    collectionsToReturn.reserve(collectionsOnConfig.size());
    for (const auto& coll : collectionsOnConfig) {
        collectionsToReturn.push_back(coll.getNss());
    }

    return collectionsToReturn;
}

StatusWith<BSONObj> ShardingCatalogClientImpl::getGlobalSettings(OperationContext* opCtx,
                                                                 StringData key) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              NamespaceString::kConfigSettingsNamespace,
                                              BSON("_id" << key),
                                              BSONObj(),
                                              1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;
    if (docs.empty()) {
        return {ErrorCodes::NoMatchingDocument,
                str::stream() << "can't find settings document with key: " << key};
    }

    invariant(docs.size() == 1);
    return docs.front();
}

StatusWith<VersionType> ShardingCatalogClientImpl::getConfigVersion(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    auto findStatus = _getConfigShard(opCtx)->exhaustiveFindOnConfig(opCtx,
                                                                     kConfigReadSelector,
                                                                     readConcern,
                                                                     VersionType::ConfigNS,
                                                                     BSONObj(),
                                                                     BSONObj(),
                                                                     boost::none /* no limit */);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    auto queryResults = findStatus.getValue().docs;

    if (queryResults.size() > 1) {
        return {ErrorCodes::TooManyMatchingDocuments,
                str::stream() << "should only have 1 document in " << VersionType::ConfigNS.ns()};
    }

    if (queryResults.empty()) {
        return {ErrorCodes::NoMatchingDocument,
                str::stream() << "No documents found in " << VersionType::ConfigNS.ns()};
    }

    BSONObj versionDoc = queryResults.front();
    auto versionTypeResult = VersionType::fromBSON(versionDoc);
    if (!versionTypeResult.isOK()) {
        return versionTypeResult.getStatus().withContext(
            str::stream() << "Unable to parse config.version document " << versionDoc);
    }

    auto validationStatus = versionTypeResult.getValue().validate();
    if (!validationStatus.isOK()) {
        return Status(validationStatus.withContext(
            str::stream() << "Unable to validate config.version document " << versionDoc));
    }

    return versionTypeResult.getValue();
}

StatusWith<std::vector<std::string>> ShardingCatalogClientImpl::getDatabasesForShard(
    OperationContext* opCtx, const ShardId& shardId) {
    auto findStatus =
        _exhaustiveFindOnConfig(opCtx,
                                kConfigReadSelector,
                                repl::ReadConcernLevel::kMajorityReadConcern,
                                NamespaceString::kConfigDatabasesNamespace,
                                BSON(DatabaseType::kPrimaryFieldName << shardId.toString()),
                                BSONObj(),
                                boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const std::vector<BSONObj>& values = findStatus.getValue().value;
    std::vector<std::string> dbs;
    dbs.reserve(values.size());
    for (const BSONObj& obj : values) {
        std::string dbName;
        Status status = bsonExtractStringField(obj, DatabaseType::kNameFieldName, &dbName);
        if (!status.isOK()) {
            return status;
        }

        dbs.push_back(std::move(dbName));
    }

    return dbs;
}

StatusWith<std::vector<ChunkType>> ShardingCatalogClientImpl::getChunks(
    OperationContext* opCtx,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<int> limit,
    repl::OpTime* opTime,
    const OID& epoch,
    const Timestamp& timestamp,
    repl::ReadConcernLevel readConcern,
    const boost::optional<BSONObj>& hint) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ||
              readConcern == repl::ReadConcernLevel::kMajorityReadConcern);

    // Convert boost::optional<int> to boost::optional<long long>.
    auto longLimit = limit ? boost::optional<long long>(*limit) : boost::none;
    auto findStatus = _exhaustiveFindOnConfig(
        opCtx, kConfigReadSelector, readConcern, ChunkType::ConfigNS, query, sort, longLimit, hint);
    if (!findStatus.isOK()) {
        return findStatus.getStatus().withContext("Failed to load chunks");
    }

    const auto& chunkDocsOpTimePair = findStatus.getValue();

    std::vector<ChunkType> chunks;
    chunks.reserve(chunkDocsOpTimePair.value.size());
    for (const BSONObj& obj : chunkDocsOpTimePair.value) {
        auto chunkRes = ChunkType::parseFromConfigBSON(obj, epoch, timestamp);
        if (!chunkRes.isOK()) {
            return chunkRes.getStatus().withContext(
                str::stream() << "Failed to parse chunk with id " << obj[ChunkType::name()]);
        }

        chunks.push_back(std::move(chunkRes.getValue()));
    }

    if (opTime) {
        *opTime = chunkDocsOpTimePair.opTime;
    }

    return chunks;
}

std::pair<CollectionType, std::vector<ChunkType>> ShardingCatalogClientImpl::getCollectionAndChunks(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& sinceVersion,
    const repl::ReadConcernArgs& readConcern) {
    auto aggRequest = makeCollectionAndChunksAggregation(opCtx, nss, sinceVersion);

    std::vector<BSONObj> aggResult = runCatalogAggregation(
        opCtx, aggRequest, readConcern, Milliseconds(gFindChunksOnConfigTimeoutMS.load()));

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << nss.ns() << " not found",
            !aggResult.empty());


    // The aggregation may return the config.collections document anywhere between the
    // config.chunks documents.
    // 1st: look for the collection since it is needed to properly build the chunks.
    boost::optional<CollectionType> coll;
    {
        for (const auto& elem : aggResult) {
            const auto chunkElem = elem.getField("chunks");
            if (!chunkElem) {
                coll.emplace(elem);
                break;
            }
        }
        uassert(5520101, "'collections' document not found in aggregation response", coll);
    }

    // 2nd: Traverse all the elements and build the chunks.
    std::vector<ChunkType> chunks;
    {
        chunks.reserve(aggResult.size() - 1);
        bool foundCollection = false;
        for (const auto& elem : aggResult) {
            const auto chunkElem = elem.getField("chunks");
            if (chunkElem) {
                auto chunkRes = uassertStatusOK(ChunkType::parseFromConfigBSON(
                    chunkElem.Obj(), coll->getEpoch(), coll->getTimestamp()));
                chunks.emplace_back(std::move(chunkRes));
            } else {
                uassert(5520100,
                        "Found more than one 'collections' documents in aggregation response",
                        !foundCollection);
                foundCollection = true;
            }
        }

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "No chunks were found for the collection " << nss,
                !chunks.empty());
    }


    return {std::move(*coll), std::move(chunks)};
};

std::pair<CollectionType, std::vector<IndexCatalogType>>
ShardingCatalogClientImpl::getCollectionAndShardingIndexCatalogEntries(
    OperationContext* opCtx, const NamespaceString& nss, const repl::ReadConcernArgs& readConcern) {
    auto aggRequest = makeCollectionAndIndexesAggregation(opCtx, nss);

    std::vector<BSONObj> aggResult = runCatalogAggregation(opCtx, aggRequest, readConcern);

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << nss.ns() << " not found",
            !aggResult.empty());

    uassert(6958800,
            str::stream() << "More than one collection for ns " << nss.ns() << " found",
            aggResult.size() == 1);

    boost::optional<CollectionType> coll;
    std::vector<IndexCatalogType> indexes;

    auto elem = aggResult[0];
    coll.emplace(elem);
    const auto indexList = elem.getField("indexes");
    for (const auto index : indexList.Array()) {
        indexes.emplace_back(
            IndexCatalogType::parse(IDLParserContext("IndexCatalogType"), index.Obj()));
    }

    return {std::move(*coll), std::move(indexes)};
}

StatusWith<std::vector<TagsType>> ShardingCatalogClientImpl::getTagsForCollection(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              TagsType::ConfigNS,
                                              BSON(TagsType::ns(nss.ns())),
                                              BSON(TagsType::min() << 1),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus().withContext("Failed to load tags");
    }

    const auto& tagDocsOpTimePair = findStatus.getValue();

    std::vector<TagsType> tags;
    tags.reserve(tagDocsOpTimePair.value.size());
    for (const BSONObj& obj : tagDocsOpTimePair.value) {
        auto tagRes = TagsType::fromBSON(obj);
        if (!tagRes.isOK()) {
            return tagRes.getStatus().withContext(str::stream() << "Failed to parse tag with id "
                                                                << obj[TagsType::tag()]);
        }

        tags.push_back(tagRes.getValue());
    }

    return tags;
}

std::vector<NamespaceString> ShardingCatalogClientImpl::getAllNssThatHaveZonesForDatabase(
    OperationContext* opCtx, const StringData& dbName) {
    auto expCtx =
        make_intrusive<ExpressionContext>(opCtx, nullptr /*collator*/, TagsType::ConfigNS);
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[TagsType::ConfigNS.coll()] = {TagsType::ConfigNS,
                                                     std::vector<BSONObj>() /* pipeline */};
    expCtx->setResolvedNamespaces(resolvedNamespaces);

    // Parse pipeline:
    //      - First stage will find all that namespaces on 'config.tags' that are part of the
    //      given database.
    //      - Second stage will group namespaces to not have repetitions.
    //
    //      db.tags.aggregate([
    //          {$match: {ns: {$regex : "^dbName\\..*"}}},
    //          {$group: {_id : "$ns"}}
    //      ])
    //
    const std::string regex = "^" + pcre_util::quoteMeta(dbName) + "\\..*";
    auto matchStageBson = BSON("ns" << BSON("$regex" << regex));
    auto matchStage = DocumentSourceMatch::createFromBson(
        Document{{"$match", std::move(matchStageBson)}}.toBson().firstElement(), expCtx);

    auto groupStageBson = BSON("_id"
                               << "$ns");
    auto groupStage = DocumentSourceGroup::createFromBson(
        Document{{"$group", std::move(groupStageBson)}}.toBson().firstElement(), expCtx);

    // Create pipeline
    Pipeline::SourceContainer stages;
    stages.emplace_back(std::move(matchStage));
    stages.emplace_back(std::move(groupStage));

    const auto pipeline = Pipeline::create(stages, expCtx);
    auto aggRequest = AggregateCommandRequest(TagsType::ConfigNS, pipeline->serializeToBson());

    // Run the aggregation
    const auto readConcern = [&]() -> repl::ReadConcernArgs {
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            return {repl::ReadConcernLevel::kMajorityReadConcern};
        } else {
            const auto time = VectorClock::get(opCtx)->getTime();
            return {time.configTime(), repl::ReadConcernLevel::kMajorityReadConcern};
        }
    }();

    auto aggResult = runCatalogAggregation(opCtx, aggRequest, readConcern);

    // Parse the result
    std::vector<NamespaceString> nssList;
    for (const auto& doc : aggResult) {
        nssList.push_back(NamespaceString(doc.getField("_id").String()));
    }
    return nssList;
}

StatusWith<repl::OpTimeWith<std::vector<ShardType>>> ShardingCatalogClientImpl::getAllShards(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              readConcern,
                                              NamespaceString::kConfigsvrShardsNamespace,
                                              BSONObj(),     // no query filter
                                              BSONObj(),     // no sort
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    std::vector<ShardType> shards;
    shards.reserve(findStatus.getValue().value.size());
    for (const BSONObj& doc : findStatus.getValue().value) {
        auto shardRes = ShardType::fromBSON(doc);
        if (!shardRes.isOK()) {
            return shardRes.getStatus().withContext(str::stream()
                                                    << "Failed to parse shard document " << doc);
        }

        Status validateStatus = shardRes.getValue().validate();
        if (!validateStatus.isOK()) {
            return validateStatus.withContext(str::stream()
                                              << "Failed to validate shard document " << doc);
        }

        shards.push_back(shardRes.getValue());
    }

    return repl::OpTimeWith<std::vector<ShardType>>{std::move(shards),
                                                    findStatus.getValue().opTime};
}

Status ShardingCatalogClientImpl::runUserManagementWriteCommand(OperationContext* opCtx,
                                                                StringData commandName,
                                                                StringData dbname,
                                                                const BSONObj& cmdObj,
                                                                BSONObjBuilder* result) {
    BSONObj cmdToRun = cmdObj;
    {
        // Make sure that if the command has a write concern that it is w:1 or w:majority, and
        // convert w:1 or no write concern to w:majority before sending.
        WriteConcernOptions writeConcern;

        BSONElement writeConcernElement = cmdObj[WriteConcernOptions::kWriteConcernField];
        bool initialCmdHadWriteConcern = !writeConcernElement.eoo();
        if (initialCmdHadWriteConcern) {
            auto sw = WriteConcernOptions::parse(writeConcernElement.Obj());
            if (!sw.isOK()) {
                return sw.getStatus();
            }
            writeConcern = sw.getValue();

            auto isValidUserManagementWriteConcern = stdx::visit(
                [](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, std::string>)
                        return arg == WriteConcernOptions::kMajority;
                    else if constexpr (std::is_same_v<T, int64_t>)
                        return arg == 1;
                    return false;
                },
                writeConcern.w);

            if (!isValidUserManagementWriteConcern) {
                return {ErrorCodes::InvalidOptions,
                        str::stream() << "Invalid replication write concern. User management write "
                                         "commands may only use w:1 or w:'majority', got: "
                                      << writeConcern.toBSON()};
            }
        }

        writeConcern.w = WriteConcernOptions::kMajority;

        BSONObjBuilder modifiedCmd;
        if (!initialCmdHadWriteConcern) {
            modifiedCmd.appendElements(cmdObj);
        } else {
            BSONObjIterator cmdObjIter(cmdObj);
            while (cmdObjIter.more()) {
                BSONElement e = cmdObjIter.next();
                if (WriteConcernOptions::kWriteConcernField == e.fieldName()) {
                    continue;
                }
                modifiedCmd.append(e);
            }
        }
        modifiedCmd.append(WriteConcernOptions::kWriteConcernField, writeConcern.toBSON());
        cmdToRun = modifiedCmd.obj();
    }

    auto swResponse = _getConfigShard(opCtx)->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        dbname.toString(),
        cmdToRun,
        Shard::kDefaultConfigCommandTimeout,
        Shard::RetryPolicy::kNotIdempotent);

    if (!swResponse.isOK()) {
        return swResponse.getStatus();
    }

    auto response = std::move(swResponse.getValue());

    if (!response.commandStatus.isOK()) {
        return response.commandStatus;
    }

    if (!response.writeConcernStatus.isOK()) {
        return response.writeConcernStatus;
    }

    CommandHelpers::filterCommandReplyForPassthrough(response.response, result);
    return Status::OK();
}

bool ShardingCatalogClientImpl::runUserManagementReadCommand(OperationContext* opCtx,
                                                             const std::string& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    auto resultStatus = _getConfigShard(opCtx)->runCommandWithFixedRetryAttempts(
        opCtx,
        kConfigPrimaryPreferredSelector,
        dbname,
        cmdObj,
        Shard::kDefaultConfigCommandTimeout,
        Shard::RetryPolicy::kIdempotent);
    if (resultStatus.isOK()) {
        CommandHelpers::filterCommandReplyForPassthrough(resultStatus.getValue().response, result);
        return resultStatus.getValue().commandStatus.isOK();
    }

    return CommandHelpers::appendCommandStatusNoThrow(*result, resultStatus.getStatus());  // XXX
}

Status ShardingCatalogClientImpl::insertConfigDocument(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const BSONObj& doc,
                                                       const WriteConcernOptions& writeConcern) {
    invariant(nss.dbName() == DatabaseName::kAdmin || nss.dbName() == DatabaseName::kConfig);

    const BSONElement idField = doc.getField("_id");

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({doc});
        return insertOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    const auto configShard = _getConfigShard(opCtx);
    for (int retry = 1; retry <= kMaxWriteRetry; retry++) {
        auto response = configShard->runBatchWriteCommand(
            opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kNoRetry);

        Status status = response.toStatus();

        if (retry < kMaxWriteRetry &&
            configShard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent)) {
            // Pretend like the operation is idempotent because we're handling DuplicateKey errors
            // specially
            continue;
        }

        // If we get DuplicateKey error on the first attempt to insert, this definitively means that
        // we are trying to insert the same entry a second time, so error out. If it happens on a
        // retry attempt though, it is not clear whether we are actually inserting a duplicate key
        // or it is because we failed to wait for write concern on the first attempt. In order to
        // differentiate, fetch the entry and check.
        if (retry > 1 && status == ErrorCodes::DuplicateKey) {
            LOGV2_DEBUG(
                22674, 1, "Insert retry failed because of duplicate key error, rechecking.");

            auto fetchDuplicate =
                _exhaustiveFindOnConfig(opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        repl::ReadConcernLevel::kMajorityReadConcern,
                                        nss,
                                        idField.eoo() ? doc : idField.wrap(),
                                        BSONObj(),
                                        boost::none);
            if (!fetchDuplicate.isOK()) {
                return fetchDuplicate.getStatus();
            }

            auto existingDocs = fetchDuplicate.getValue().value;
            if (existingDocs.empty()) {
                return {
                    status.withContext(
                        str::stream()
                        << "DuplicateKey error was returned after a retry attempt, but no "
                           "documents were found. This means a concurrent change occurred "
                           "together with the retries.")};
            }

            invariant(existingDocs.size() == 1);

            BSONObj existing = std::move(existingDocs.front());
            if (existing.woCompare(doc) == 0) {
                // Documents match, so treat the operation as success
                return Status::OK();
            }
        }

        return status;
    }

    MONGO_UNREACHABLE;
}

StatusWith<bool> ShardingCatalogClientImpl::updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    return _updateConfigDocument(
        opCtx, nss, query, update, upsert, writeConcern, Shard::kDefaultConfigCommandTimeout);
}

StatusWith<bool> ShardingCatalogClientImpl::updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern,
    Milliseconds maxTimeMs) {
    return _updateConfigDocument(opCtx, nss, query, update, upsert, writeConcern, maxTimeMs);
}

StatusWith<bool> ShardingCatalogClientImpl::_updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern,
    Milliseconds maxTimeMs) {
    invariant(nss.dbName() == DatabaseName::kConfig);

    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setUpsert(upsert);
            entry.setMulti(false);
            return entry;
        }()});
        return updateOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    auto response = _getConfigShard(opCtx)->runBatchWriteCommand(
        opCtx, maxTimeMs, request, Shard::RetryPolicy::kIdempotent);

    Status status = response.toStatus();
    if (!status.isOK()) {
        return status;
    }

    const auto nSelected = response.getN();
    invariant(nSelected == 0 || nSelected == 1);
    return (nSelected == 1);
}

Status ShardingCatalogClientImpl::removeConfigDocuments(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        const BSONObj& query,
                                                        const WriteConcernOptions& writeConcern,
                                                        boost::optional<BSONObj> hint) {
    invariant(nss.db() == DatabaseName::kConfig.db());

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            if (hint) {
                entry.setHint(*hint);
            }
            entry.setMulti(true);
            return entry;
        }()});
        return deleteOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    auto response = _getConfigShard(opCtx)->runBatchWriteCommand(
        opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);
    return response.toStatus();
}

StatusWith<repl::OpTimeWith<std::vector<BSONObj>>>
ShardingCatalogClientImpl::_exhaustiveFindOnConfig(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& readPref,
                                                   const repl::ReadConcernLevel& readConcern,
                                                   const NamespaceString& nss,
                                                   const BSONObj& query,
                                                   const BSONObj& sort,
                                                   boost::optional<long long> limit,
                                                   const boost::optional<BSONObj>& hint) {
    auto response = _getConfigShard(opCtx)->exhaustiveFindOnConfig(
        opCtx, readPref, readConcern, nss, query, sort, limit, hint);
    if (!response.isOK()) {
        return response.getStatus();
    }

    return repl::OpTimeWith<std::vector<BSONObj>>(std::move(response.getValue().docs),
                                                  response.getValue().opTime);
}

StatusWith<std::vector<KeysCollectionDocument>> ShardingCatalogClientImpl::getNewKeys(
    OperationContext* opCtx,
    StringData purpose,
    const LogicalTime& newerThanThis,
    repl::ReadConcernLevel readConcernLevel) {
    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", purpose);
    queryBuilder.append("expiresAt", BSON("$gt" << newerThanThis.asTimestamp()));

    auto findStatus =
        _getConfigShard(opCtx)->exhaustiveFindOnConfig(opCtx,
                                                       kConfigReadSelector,
                                                       readConcernLevel,
                                                       NamespaceString::kKeysCollectionNamespace,
                                                       queryBuilder.obj(),
                                                       BSON("expiresAt" << 1),
                                                       boost::none);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& keyDocs = findStatus.getValue().docs;
    std::vector<KeysCollectionDocument> keys;
    keys.reserve(keyDocs.size());
    for (auto&& keyDoc : keyDocs) {
        try {
            keys.push_back(KeysCollectionDocument::parse(IDLParserContext("keyDoc"), keyDoc));
        } catch (...) {
            return exceptionToStatus();
        }
    }

    return keys;
}


HistoricalPlacement ShardingCatalogClientImpl::getShardsThatOwnDataForCollAtClusterTime(
    OperationContext* opCtx, const NamespaceString& collName, const Timestamp& clusterTime) {

    uassert(ErrorCodes::InvalidOptions,
            "A full collection namespace must be specified",
            !collName.coll().empty());

    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        return getHistoricalPlacement(opCtx, clusterTime, collName);
    }

    return _fetchPlacementMetadata(opCtx, ConfigsvrGetHistoricalPlacement(collName, clusterTime));
}


HistoricalPlacement ShardingCatalogClientImpl::getShardsThatOwnDataForDbAtClusterTime(
    OperationContext* opCtx, const NamespaceString& dbName, const Timestamp& clusterTime) {

    uassert(ErrorCodes::InvalidOptions,
            "A full db namespace must be specified",
            dbName.coll().empty() && !dbName.db().empty());

    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        return getHistoricalPlacement(opCtx, clusterTime, dbName);
    }

    return _fetchPlacementMetadata(opCtx, ConfigsvrGetHistoricalPlacement(dbName, clusterTime));
}

HistoricalPlacement ShardingCatalogClientImpl::getShardsThatOwnDataAtClusterTime(
    OperationContext* opCtx, const Timestamp& clusterTime) {

    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        return getHistoricalPlacement(opCtx, clusterTime, boost::none);
    }

    ConfigsvrGetHistoricalPlacement request(NamespaceString(), clusterTime);
    request.setTargetWholeCluster(true);
    return _fetchPlacementMetadata(opCtx, std::move(request));
}

HistoricalPlacement ShardingCatalogClientImpl::getHistoricalPlacement(
    OperationContext* opCtx,
    const Timestamp& atClusterTime,
    const boost::optional<NamespaceString>& nss) {

    // TODO (SERVER-73029): Remove the invariant
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
    auto configShard = _getConfigShard(opCtx);
    /*
    The aggregation pipeline is split in 2 sub pipelines:
    - one pipeline "exactPlacementData" describing the list of shards currently active in the
    cluster in which the data belonging to the input nss were placed at the given clusterTime. The
    primary shard is always included in the list. In case the input nss is empty, the list of shards
    includes all the shards in the cluster containing data at the given clusterTime. Stages can be
    described as follow:
        Stage 1. Select only the entry with timestamp <= clusterTime and filter out
    all nss that are not the collection or the database using a regex. We also esclude all the
    entries related to the fcv marker. In case the whole cluster info is searched, we filter any nss
    with at least 1 caracter
        Stage 2. sort by timestamp
        Stage 3. Extract the first document for each database and collection matching the received
    namespace
        Stage 4. Discard the entries with empty shards (i.e. the collection was dropped or
    renamed)
        Stage 5. Group all documents and concat shards (this will generate an array of arrays)
        Stage 6. Flatten the array of arrays into a set
    (this will also remove duplicates)
        Stage 7. Access to the list of shards currently active in the cluster
    - one pipeline "approximatePlacementData" retreiving the last "marker" which is a special entry
    where the nss is empty and the list of shard can be either empty or not.
        - In case the list is not empty: it means the clusterTime requested was during an fcv
    upgrade/downgrade. Thus we cannot guarantee the result of 'exactPlacementData' to
    be correct. We therefore report the list of shards present in the "marker" entry, which
    correspond to the list of shards in the cluster at the time the fcv upgrade/downgrade started.
        - The pipeline selects only the fcv markers, sorts by decreasing timestamp and gets the
    first element.

    regex=^db(\.collection)?$ // matches db or db.collection
    db.placementHistory.aggregate([
      {
        "$facet": {
          "exactPlacementData": [
            {
              "$match": {
                "timestamp": {
                  "$lte":<clusterTime>
                },
                "nss": {
                  $regex: regex
                }
              }
            },
            {
              "$sort": {
                "timestamp": -1
              }
            },
            {
              "$group": {
                _id: "$nss",
                shards: {
                  $first: "$shards"
                }
              }
            },
            {
              "$match": {
                shards: {
                  $not: {
                    $size: 0
                  }
                }
              }
            },
            {
              "$group": {
                _id: "",
                shards: {
                  $push: "$shards"
                }
              }
            },
            {
              $project: {
                "shards": {
                  $reduce: {
                    input: "$shards",
                    initialValue: [],
                    in: {
                      "$setUnion": [
                        "$$this",
                        "$$value"
                      ]
                    }
                  }
                }
              }
            }
          ],
          "approximatePlacementData": [
            {
              "$match": {
                "timestamp": {
                  "$lte": <clusterTime>
                },
                "nss": kConfigsvrPlacementHistoryFcvMarkerNamespace
              }
            },
            {
              "$sort": {
                "timestamp": -1
              }
            },
            {
              "$limit": 1
            }
          ]
        }
      }
    ])

        */

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, nullptr /*collator*/, NamespaceString::kConfigsvrPlacementHistoryNamespace);
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[NamespaceString::kConfigsvrShardsNamespace.coll()] = {
        NamespaceString::kConfigsvrShardsNamespace, std::vector<BSONObj>() /* pipeline */};
    resolvedNamespaces[NamespaceString::kConfigsvrPlacementHistoryNamespace.coll()] = {
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        std::vector<BSONObj>() /* pipeline */};
    expCtx->setResolvedNamespaces(resolvedNamespaces);

    // Build the pipeline for the exact placement data.
    // 1. Get all the history entries prior to the requested time concerning either the collection
    // or the parent database.
    const auto& kMarkerNss = NamespaceString::kConfigsvrPlacementHistoryFcvMarkerNamespace.ns();
    auto matchStage = [&]() {
        bool isClusterSearch = !nss.has_value();
        if (isClusterSearch)
            return DocumentSourceMatch::create(BSON("nss" << BSON("$ne" << kMarkerNss)
                                                          << "timestamp"
                                                          << BSON("$lte" << atClusterTime)),
                                               expCtx);

        bool isCollectionSearch = !nss->db().empty() && !nss->coll().empty();
        auto collMatchExpression = isCollectionSearch ? pcre_util::quoteMeta(nss->coll()) : ".*";
        auto regexString =
            "^" + pcre_util::quoteMeta(nss->db()) + "(\\." + collMatchExpression + ")?$";
        return DocumentSourceMatch::create(BSON("nss" << BSON("$regex" << regexString)
                                                      << "timestamp"
                                                      << BSON("$lte" << atClusterTime)),
                                           expCtx);
    }();

    // 2 & 3. Sort by timestamp and extract the first document for collection and database
    auto sortStage = DocumentSourceSort::create(expCtx, BSON("timestamp" << -1));
    auto groupStageBson = BSON("_id"
                               << "$nss"
                               << "shards"
                               << BSON("$first"
                                       << "$shards"));
    auto groupStage = DocumentSourceGroup::createFromBson(
        Document{{"$group", std::move(groupStageBson)}}.toBson().firstElement(), expCtx);

    // Stage 4. Discard the entries with empty shards (i.e. the collection was dropped or renamed)
    auto noShardsFilter =
        DocumentSourceMatch::create(BSON("shards" << BSON("$not" << BSON("$size" << 0))), expCtx);

    // Stage 5. Group all documents and concat shards (this will generate an array of arrays)
    auto groupStageBson2 = BSON("_id"
                                << ""
                                << "shards"
                                << BSON("$push"
                                        << "$shards"));
    auto groupStageConcat = DocumentSourceGroup::createFromBson(
        Document{{"$group", std::move(groupStageBson2)}}.toBson().firstElement(), expCtx);

    // Stage 6. Flatten the array of arrays into a set (this will also remove duplicates)
    auto projectStageBson =
        BSON("shards" << BSON("$reduce" << BSON("input"
                                                << "$shards"
                                                << "initialValue" << BSONArray() << "in"
                                                << BSON("$setUnion" << BSON_ARRAY("$$this"
                                                                                  << "$$value")))));
    auto projectStageFlatten = DocumentSourceProject::createFromBson(
        Document{{"$project", std::move(projectStageBson)}}.toBson().firstElement(), expCtx);

    Pipeline::SourceContainer stages;
    stages.emplace_back(std::move(matchStage));
    stages.emplace_back(std::move(sortStage));
    stages.emplace_back(std::move(groupStage));
    stages.emplace_back(std::move(noShardsFilter));
    stages.emplace_back(std::move(groupStageConcat));
    stages.emplace_back(std::move(projectStageFlatten));
    auto exactDataPipeline = Pipeline::create(stages, expCtx);

    // Build the pipeline for the approximate data.
    auto matchFcvMarkerStage = DocumentSourceMatch::create(
        BSON("timestamp" << BSON("$lte" << atClusterTime) << "nss" << kMarkerNss), expCtx);
    auto sortFcvMarkerStage = DocumentSourceSort::create(expCtx, BSON("timestamp" << -1));
    auto limitFcvMarkerStage = DocumentSourceLimit::create(expCtx, 1);
    Pipeline::SourceContainer stages2;
    stages2.emplace_back(std::move(matchFcvMarkerStage));
    stages2.emplace_back(std::move(sortFcvMarkerStage));
    stages2.emplace_back(std::move(limitFcvMarkerStage));
    auto approximateDataPipeline = Pipeline::create(stages2, expCtx);


    // Build the facet pipeline
    auto facetStageBson = BSON("approximatePlacementData"
                               << approximateDataPipeline->serializeToBson() << "exactPlacementData"
                               << exactDataPipeline->serializeToBson());
    auto facetStage = DocumentSourceFacet::createFromBson(
        Document{{"$facet", std::move(facetStageBson)}}.toBson().firstElement(), expCtx);

    const auto pipeline = Pipeline::create({facetStage}, expCtx);
    auto aggRequest = AggregateCommandRequest(NamespaceString::kConfigsvrPlacementHistoryNamespace,
                                              pipeline->serializeToBson());


    // Run the aggregation
    const auto readConcern = [&]() -> repl::ReadConcernArgs {
        // (Ignore FCV check): This is in mongos so we expect to ignore FCV.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
            !gFeatureFlagCatalogShard.isEnabledAndIgnoreFCVUnsafe()) {
            // When the feature flag is on, the config server may read from a secondary which may
            // need to wait for replication, so we should use afterClusterTime.
            return {repl::ReadConcernLevel::kMajorityReadConcern};
        } else {
            const auto time = VectorClock::get(opCtx)->getTime();
            return {time.configTime(), repl::ReadConcernLevel::kMajorityReadConcern};
        }
    }();

    auto aggrResult = runCatalogAggregation(opCtx, aggRequest, readConcern);

    auto extractShardIds = [](const BSONObj& obj, const std::string& pipelineName) {
        // each sub-pipeline of $facet produces an array with a single element containing a 'shards'
        // field. for this aggregation, every pipeline result is an array of one element
        auto pipelineResult = obj[pipelineName].Array();
        if (pipelineResult.empty()) {
            return std::vector<ShardId>{};
        } else {
            auto shards = pipelineResult[0]["shards"].Obj();
            std::vector<ShardId> activeShards;
            for (const auto& shard : shards) {
                activeShards.push_back(shard.String());
            }
            return activeShards;
        }
    };

    invariant(aggrResult.size() == 1);
    // if there is an fcv marker and the shards array is not empty, return the shards
    // array, declaring the retrieved data as "not exact".
    auto fcvMarkerShards = extractShardIds(aggrResult.front(), "approximatePlacementData");
    if (!fcvMarkerShards.empty()) {
        return HistoricalPlacement{fcvMarkerShards, false};
    }

    // if the fcv marker shards array is empty, return the shards array from the exact data
    auto exactShards = extractShardIds(aggrResult.front(), "exactPlacementData");
    if (exactShards.empty()) {
        return HistoricalPlacement{{}, true};
    }

    return HistoricalPlacement{exactShards, true};
}

std::shared_ptr<Shard> ShardingCatalogClientImpl::_getConfigShard(OperationContext* opCtx) {
    if (_overrideConfigShard) {
        return _overrideConfigShard;
    }
    return Grid::get(opCtx)->shardRegistry()->getConfigShard();
}

}  // namespace mongo

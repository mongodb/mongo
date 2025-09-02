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

#include "mongo/db/global_catalog/sharding_catalog_client_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/global_catalog/type_config_version_gen.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_remove_shard_event_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/cluster_umc_error_with_write_concern_error_info.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/pcre_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <type_traits>
#include <variant>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const ReadPreferenceSetting kConfigNearestReadPreference(ReadPreference::Nearest, TagSet{});
const ReadPreferenceSetting kConfigPrimaryPreferredReadPreference(ReadPreference::PrimaryPreferred,
                                                                  TagSet{});
const int kMaxReadRetry = 3;
const int kMaxWriteRetry = 3;


/**
 * This function returns `true` if the `configServerReadPreferenceForCatalogQueries`
 * cluster server parameter exists and is set to indicate that the nearest read preference
 * must always be used. If the parameter is not known or is set to false, it returns `false`.
 */
bool forceUsageOfNearestReadPreference() {
    auto* configServerReadPreferenceForCatalogQueriesParam =
        ServerParameterSet::getClusterParameterSet()
            ->getIfExists<
                ClusterParameterWithStorage<ConfigServerReadPreferenceForCatalogQueriesParam>>(
                "configServerReadPreferenceForCatalogQueries");

    if (!configServerReadPreferenceForCatalogQueriesParam) {
        return false;
    }

    return configServerReadPreferenceForCatalogQueriesParam->getValue(boost::none)
        .getMustAlwaysUseNearest();
}

/**
 * Returns the appropriate read preference for queries targeting the config server:
 * - If the read preference cluster server parameter is set, it returns 'nearest'.
 * - For clusters with a config shard, it returns 'primaryPreferred'.
 * - For clusters with a dedicated config server, it returns 'nearest'.
 *
 * To check if the cluster has a config shard, the cached data is consulted without causal
 * consistency. This is because we don't want to introduce extra latency to queries targeting the
 * config server to determine a read preference.
 *
 * Note: The selection of the read preference only makes sense when using the remote catalog client
 * and not the local one.
 * TODO (SERVER-91526): Make an early exit when using the local catalog client.
 */
ReadPreferenceSetting getConfigReadPreference(OperationContext* opCtx) {
    if (forceUsageOfNearestReadPreference()) {
        return kConfigNearestReadPreference;
    }

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto optClusterHasConfigShard = shardRegistry->cachedClusterHasConfigShard();

    if (!optClusterHasConfigShard) {
        return kConfigPrimaryPreferredReadPreference;
    }

    return *optClusterHasConfigShard ? kConfigPrimaryPreferredReadPreference
                                     : kConfigNearestReadPreference;
}

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setStatus(status);
}

AggregateCommandRequest makeCollectionAndChunksAggregation(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const ChunkVersion& sinceVersion) {
    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces[CollectionType::ConfigNS] = {CollectionType::ConfigNS,
                                                    std::vector<BSONObj>()};
    resolvedNamespaces[NamespaceString::kConfigsvrChunksNamespace] = {
        NamespaceString::kConfigsvrChunksNamespace, std::vector<BSONObj>()};

    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .ns(CollectionType::ConfigNS)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .build();
    using Doc = Document;
    using Arr = std::vector<Value>;

    DocumentSourceContainer stages;

    // 1. Match config.collections entries with {_id: nss}. This stage will produce, at most, one
    // config.collections document.
    // {
    //     $match: {
    //         _id: <nss>
    //     }
    // }
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{CollectionType::kNssFieldName,
             NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())}}
            .toBson(),
        expCtx));

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
                {"from", NamespaceString::kConfigsvrChunksNamespace.coll()},
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
             Arr{Value{Doc{{"$match",
                            Doc{{CollectionType::kNssFieldName,
                                 NamespaceStringUtil::serialize(
                                     nss, SerializationContext::stateDefault())}}}}},
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

AggregateCommandRequest makeUnsplittableCollectionsDataShardAggregation(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const std::vector<ShardId>& excludedShards) {
    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces[CollectionType::ConfigNS] = {CollectionType::ConfigNS,
                                                    std::vector<BSONObj>()};
    resolvedNamespaces[NamespaceString::kConfigsvrChunksNamespace] = {
        NamespaceString::kConfigsvrChunksNamespace, std::vector<BSONObj>()};
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .ns(CollectionType::ConfigNS)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .build();
    using Doc = Document;
    using Arr = std::vector<Value>;

    DocumentSourceContainer stages;

    // 1. Match config.collections entries with database name = dbName
    // {
    //     $match: {
    //         _id: {$regex: dbName.*, unsplittable: true}
    //     }
    // }
    const auto db =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest());
    stages.emplace_back(DocumentSourceMatch::create(
        BSON(CollectionType::kNssFieldName
             << BSON("$regex" << fmt::format("^{}\\.", pcre_util::quoteMeta(db)))
             << CollectionType::kUnsplittableFieldName << true),
        expCtx));

    // 2. Retrieve config.chunks entries with the same uuid as the one from the
    // config.collections document.
    //
    // The $lookup stage gets the config.chunks documents and puts them in a field called
    // "chunks" in the document produced during stage 1.
    //
    // {
    //      $lookup: {
    //          from: "chunks",
    //          as: "chunks",
    //          localField: "uuid",
    //          foreignField: "uuid"
    //      }
    // }
    const Doc lookupPipeline{{"from", NamespaceString::kConfigsvrChunksNamespace.coll()},
                             {"as", "chunks"_sd},
                             {"localField", CollectionType::kUuidFieldName},
                             {"foreignField", CollectionType::kUuidFieldName}};

    stages.emplace_back(DocumentSourceLookUp::createFromBson(
        Doc{{"$lookup", lookupPipeline}}.toBson().firstElement(), expCtx));

    // 3. Filter only the collection entries where the chunk has the shard field equal to shardId.
    // {
    //      $match: {
    //          chunks.shard: {$nin: <excludedShards>}
    //      }
    // }
    BSONObjBuilder ninBuilder;
    ninBuilder.append("$nin", excludedShards);
    stages.emplace_back(
        DocumentSourceMatch::create(Doc{{"chunks.shard", ninBuilder.obj()}}.toBson(), expCtx));

    auto pipeline = Pipeline::create(std::move(stages), expCtx);
    auto serializedPipeline = pipeline->serializeToBson();
    return AggregateCommandRequest(CollectionType::ConfigNS, std::move(serializedPipeline));
}

/**
 * Returns keys for the given purpose and have an expiresAt value greater than newerThanThis on the
 * given shard.
 */
template <typename KeyDocumentType>
StatusWith<std::vector<KeyDocumentType>> _getNewKeys(OperationContext* opCtx,
                                                     std::shared_ptr<Shard> shard,
                                                     const NamespaceString& nss,
                                                     StringData purpose,
                                                     const LogicalTime& newerThanThis,
                                                     repl::ReadConcernLevel readConcernLevel) {
    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", purpose);
    queryBuilder.append("expiresAt", BSON("$gt" << newerThanThis.asTimestamp()));

    auto findStatus = shard->exhaustiveFindOnConfig(opCtx,
                                                    getConfigReadPreference(opCtx),
                                                    readConcernLevel,
                                                    nss,
                                                    queryBuilder.obj(),
                                                    BSON("expiresAt" << 1),
                                                    boost::none);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }
    const auto& objs = findStatus.getValue().docs;

    std::vector<KeyDocumentType> keyDocs;
    keyDocs.reserve(objs.size());
    for (auto&& obj : objs) {
        try {
            keyDocs.push_back(KeyDocumentType::parse(obj, IDLParserContext("keyDoc")));
        } catch (...) {
            return exceptionToStatus();
        }
    }
    return keyDocs;
}

}  // namespace

ShardingCatalogClientImpl::ShardingCatalogClientImpl(std::shared_ptr<Shard> overrideConfigShard)
    : _overrideConfigShard(std::move(overrideConfigShard)) {}

ShardingCatalogClientImpl::~ShardingCatalogClientImpl() = default;

DatabaseType ShardingCatalogClientImpl::getDatabase(OperationContext* opCtx,
                                                    const DatabaseName& dbName,
                                                    repl::ReadConcernLevel readConcernLevel) {
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << dbName.toStringForErrorMsg() << " is not a valid db name",
            DatabaseName::isValid(dbName, DatabaseName::DollarInDbNameBehavior::Allow));

    // The admin database is always hosted on the config server.
    if (dbName.isAdminDB()) {
        return DatabaseType(dbName, ShardId::kConfigServerId, DatabaseVersion::makeFixed());
    }

    // The config database's primary shard is always config, and it is always sharded.
    if (dbName.isConfigDB()) {
        return DatabaseType(dbName, ShardId::kConfigServerId, DatabaseVersion::makeFixed());
    }

    auto result =
        _fetchDatabaseMetadata(opCtx, dbName, getConfigReadPreference(opCtx), readConcernLevel);
    if (result == ErrorCodes::NamespaceNotFound) {
        // If we failed to find the database metadata on the 'nearest' config server, try again
        // against the primary, in case the database was recently created.
        return uassertStatusOK(
                   _fetchDatabaseMetadata(opCtx,
                                          dbName,
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          readConcernLevel))
            .value;
    }

    return uassertStatusOK(std::move(result)).value;
}

std::vector<DatabaseType> ShardingCatalogClientImpl::getAllDBs(OperationContext* opCtx,
                                                               repl::ReadConcernLevel readConcern) {
    auto dbs = uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                       getConfigReadPreference(opCtx),
                                                       readConcern,
                                                       NamespaceString::kConfigDatabasesNamespace,
                                                       BSONObj(),
                                                       BSONObj(),
                                                       boost::none))
                   .value;

    std::vector<DatabaseType> databases;
    databases.reserve(dbs.size());
    for (const BSONObj& doc : dbs) {
        databases.emplace_back(DatabaseType::parse(doc, IDLParserContext("DatabaseType")));
    }

    return databases;
}

StatusWith<repl::OpTimeWith<DatabaseType>> ShardingCatalogClientImpl::_fetchDatabaseMetadata(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const ReadPreferenceSetting& readPref,
    repl::ReadConcernLevel readConcernLevel) {
    invariant(!dbName.isAdminDB() && !dbName.isConfigDB());

    auto findStatus =
        _exhaustiveFindOnConfig(opCtx,
                                readPref,
                                readConcernLevel,
                                NamespaceString::kConfigDatabasesNamespace,
                                BSON(DatabaseType::kDbNameFieldName << DatabaseNameUtil::serialize(
                                         dbName, SerializationContext::stateCommandRequest())),
                                BSONObj(),
                                boost::none);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docsWithOpTime = findStatus.getValue();
    if (docsWithOpTime.value.empty()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName.toStringForErrorMsg() << " not found"};
    }

    invariant(docsWithOpTime.value.size() == 1);

    try {
        auto db =
            DatabaseType::parse(docsWithOpTime.value.front(), IDLParserContext("DatabaseType"));
        return repl::OpTimeWith<DatabaseType>(db, docsWithOpTime.opTime);
    } catch (const DBException& e) {
        return e.toStatus("Failed to parse DatabaseType");
    }
}

HistoricalPlacement ShardingCatalogClientImpl::_fetchPlacementMetadata(
    OperationContext* opCtx, ConfigsvrGetHistoricalPlacement&& request) {
    auto remoteResponse = uassertStatusOK(
        _getConfigShard(opCtx)->runCommand(opCtx,
                                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                           DatabaseName::kAdmin,
                                           request.toBSON(),
                                           Milliseconds(defaultConfigCommandTimeoutMS.load()),
                                           Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    uassertStatusOK(remoteResponse.commandStatus);

    auto placementDetails = ConfigsvrGetHistoricalPlacementResponse::parse(
        remoteResponse.response, IDLParserContext("ShardingCatalogClient"));

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

    aggRequest.setReadConcern(readConcern);
    aggRequest.setWriteConcern(WriteConcernOptions());

    const auto readPref = [&]() -> ReadPreferenceSetting {
        const auto vcTime = VectorClock::get(opCtx)->getTime();
        ReadPreferenceSetting readPref = getConfigReadPreference(opCtx);
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
        uassertStatusOK(_exhaustiveFindOnConfig(
                            opCtx,
                            getConfigReadPreference(opCtx),
                            readConcernLevel,
                            CollectionType::ConfigNS,
                            BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                                     nss, SerializationContext::stateDefault())),
                            BSONObj(),
                            1))
            .value;
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection '" << nss.toStringForErrorMsg()
                          << "' not found in cluster catalog",
            !collDoc.empty());

    return CollectionType(collDoc[0]);
}

CollectionType ShardingCatalogClientImpl::getCollection(OperationContext* opCtx,
                                                        const UUID& uuid,
                                                        repl::ReadConcernLevel readConcernLevel) {
    auto collDoc =
        uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                getConfigReadPreference(opCtx),
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
std::vector<CollectionType> ShardingCatalogClientImpl::getShardedCollections(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernLevel readConcernLevel,
    const BSONObj& sort) {
    BSONObjBuilder b;
    if (!dbName.isEmpty()) {
        const auto db =
            DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest());
        b.appendRegex(CollectionType::kNssFieldName,
                      fmt::format("^{}\\.", pcre_util::quoteMeta(db)));
    }

    b.append(CollectionType::kUnsplittableFieldName, BSON("$ne" << true));

    auto collDocs = uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                            getConfigReadPreference(opCtx),
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

std::vector<CollectionType> ShardingCatalogClientImpl::getCollections(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernLevel readConcernLevel,
    const BSONObj& sort) {
    BSONObjBuilder b;
    if (!dbName.isEmpty()) {
        const auto db =
            DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest());
        b.appendRegex(CollectionType::kNssFieldName,
                      fmt::format("^{}\\.", pcre_util::quoteMeta(db)));
    }

    auto collDocs = uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                            getConfigReadPreference(opCtx),
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

std::vector<NamespaceString> ShardingCatalogClientImpl::getShardedCollectionNamespacesForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernLevel readConcern,
    const BSONObj& sort) {
    BSONObjBuilder b;
    const auto db =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest());
    b.appendRegex(CollectionType::kNssFieldName, fmt::format("^{}\\.", pcre_util::quoteMeta(db)));
    b.append(CollectionTypeBase::kUnsplittableFieldName, BSON("$ne" << true));

    auto collDocs = uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                            getConfigReadPreference(opCtx),
                                                            readConcern,
                                                            CollectionType::ConfigNS,
                                                            b.obj(),
                                                            sort,
                                                            boost::none))
                        .value;

    std::vector<NamespaceString> collections;
    collections.reserve(collDocs.size());
    for (const BSONObj& obj : collDocs) {
        auto coll = CollectionTypeBase::parse(obj, IDLParserContext("getShardedCollectionsForDb"));
        collections.emplace_back(coll.getNss());
    }

    return collections;
}

std::vector<NamespaceString> ShardingCatalogClientImpl::getCollectionNamespacesForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
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

std::vector<NamespaceString> ShardingCatalogClientImpl::getUnsplittableCollectionNamespacesForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernLevel readConcern,
    const BSONObj& sort) {
    BSONObjBuilder b;
    const auto db =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest());
    b.appendRegex(CollectionType::kNssFieldName, fmt::format("^{}\\.", pcre_util::quoteMeta(db)));
    b.append(CollectionTypeBase::kUnsplittableFieldName, true);

    auto collDocs = uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                            getConfigReadPreference(opCtx),
                                                            readConcern,
                                                            CollectionType::ConfigNS,
                                                            b.obj(),
                                                            sort,
                                                            boost::none))
                        .value;

    std::vector<NamespaceString> collections;
    collections.reserve(collDocs.size());
    for (const BSONObj& obj : collDocs) {
        auto coll =
            CollectionTypeBase::parse(obj, IDLParserContext("getAllUnsplittableCollectionsForDb"));
        collections.emplace_back(coll.getNss());
    }

    return collections;
}

std::vector<NamespaceString>
ShardingCatalogClientImpl::getUnsplittableCollectionNamespacesForDbOutsideOfShards(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const std::vector<ShardId>& excludedShards,
    repl::ReadConcernLevel readConcern) {
    auto aggRequest =
        makeUnsplittableCollectionsDataShardAggregation(opCtx, dbName, excludedShards);
    std::vector<BSONObj> collectionEntries =
        Grid::get(opCtx)->catalogClient()->runCatalogAggregation(
            opCtx, aggRequest, repl::ReadConcernArgs(readConcern));
    std::vector<NamespaceString> collectionNames;
    collectionNames.reserve(collectionEntries.size());
    for (const auto& coll : collectionEntries) {
        auto nssField = coll.getField(CollectionType::kNssFieldName);
        collectionNames.push_back(NamespaceStringUtil::deserialize(
            boost::none, nssField.String(), SerializationContext::stateDefault()));
    }
    return collectionNames;
}

StatusWith<BSONObj> ShardingCatalogClientImpl::getGlobalSettings(OperationContext* opCtx,
                                                                 StringData key) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              getConfigReadPreference(opCtx),
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
    auto findStatus =
        _getConfigShard(opCtx)->exhaustiveFindOnConfig(opCtx,
                                                       getConfigReadPreference(opCtx),
                                                       readConcern,
                                                       NamespaceString::kConfigVersionNamespace,
                                                       BSONObj(),
                                                       BSONObj(),
                                                       boost::none /* no limit */);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    auto queryResults = findStatus.getValue().docs;

    if (queryResults.size() > 1) {
        return {ErrorCodes::TooManyMatchingDocuments,
                str::stream() << "should only have 1 document in "
                              << NamespaceString::kConfigVersionNamespace.toStringForErrorMsg()};
    }

    if (queryResults.empty()) {
        return {ErrorCodes::NoMatchingDocument,
                str::stream() << "No documents found in "
                              << NamespaceString::kConfigVersionNamespace.toStringForErrorMsg()};
    }

    try {
        return VersionType::parseOwned(std::move(queryResults.front()),
                                       IDLParserContext("VersionType"));
    } catch (const DBException& e) {
        return e.toStatus().withContext(str::stream() << "Unable to parse config.version document");
    }
}

StatusWith<std::vector<DatabaseName>> ShardingCatalogClientImpl::getDatabasesForShard(
    OperationContext* opCtx, const ShardId& shardId) {
    auto findStatus =
        _exhaustiveFindOnConfig(opCtx,
                                getConfigReadPreference(opCtx),
                                repl::ReadConcernLevel::kMajorityReadConcern,
                                NamespaceString::kConfigDatabasesNamespace,
                                BSON(DatabaseType::kPrimaryFieldName << shardId.toString()),
                                BSONObj(),
                                boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const std::vector<BSONObj>& values = findStatus.getValue().value;
    std::vector<DatabaseName> dbs;
    dbs.reserve(values.size());
    for (const BSONObj& obj : values) {
        std::string dbName;
        Status status = bsonExtractStringField(obj, DatabaseType::kDbNameFieldName, &dbName);
        if (!status.isOK()) {
            return status;
        }

        dbs.push_back(DatabaseNameUtil::deserialize(
            boost::none, std::move(dbName), SerializationContext::stateDefault()));
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
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              getConfigReadPreference(opCtx),
                                              readConcern,
                                              NamespaceString::kConfigsvrChunksNamespace,
                                              query,
                                              sort,
                                              longLimit,
                                              hint);
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
            str::stream() << "Collection " << nss.toStringForErrorMsg() << " not found",
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
                str::stream() << "No chunks were found for the collection "
                              << nss.toStringForErrorMsg(),
                !chunks.empty());
    }


    return {std::move(*coll), std::move(chunks)};
};

StatusWith<std::vector<TagsType>> ShardingCatalogClientImpl::getTagsForCollection(
    OperationContext* opCtx, const NamespaceString& nss, boost::optional<long long> limit) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              getConfigReadPreference(opCtx),
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              TagsType::ConfigNS,
                                              BSON(TagsType::ns(NamespaceStringUtil::serialize(
                                                  nss, SerializationContext::stateDefault()))),
                                              BSON(TagsType::min() << 1),
                                              limit);
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
    OperationContext* opCtx, const DatabaseName& dbName) {
    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces[TagsType::ConfigNS] = {TagsType::ConfigNS,
                                              std::vector<BSONObj>() /* pipeline */};
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .ns(TagsType::ConfigNS)
                      .build();
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
    const std::string regex = "^" +
        pcre_util::quoteMeta(DatabaseNameUtil::serialize(
            dbName, SerializationContext::stateCommandRequest())) +
        "\\..*";
    auto matchStageBson = BSON("ns" << BSON("$regex" << regex));
    auto matchStage = DocumentSourceMatch::createFromBson(
        Document{{"$match", std::move(matchStageBson)}}.toBson().firstElement(), expCtx);

    auto groupStageBson = BSON("_id" << "$ns");
    auto groupStage = DocumentSourceGroup::createFromBson(
        Document{{"$group", std::move(groupStageBson)}}.toBson().firstElement(), expCtx);

    // Create pipeline
    DocumentSourceContainer stages;
    stages.emplace_back(std::move(matchStage));
    stages.emplace_back(std::move(groupStage));

    const auto pipeline = Pipeline::create(stages, expCtx);
    auto aggRequest = AggregateCommandRequest(TagsType::ConfigNS, pipeline->serializeToBson());

    // Run the aggregation
    const auto readConcern = [&]() -> repl::ReadConcernArgs {
        const auto time = VectorClock::get(opCtx)->getTime();
        return {time.configTime(), repl::ReadConcernLevel::kMajorityReadConcern};
    }();

    auto aggResult = runCatalogAggregation(opCtx, aggRequest, readConcern);

    // Parse the result
    std::vector<NamespaceString> nssList;
    nssList.reserve(aggResult.size());
    for (const auto& doc : aggResult) {
        nssList.push_back(NamespaceStringUtil::deserialize(
            boost::none, doc.getField("_id").String(), SerializationContext::stateDefault()));
    }
    return nssList;
}

repl::OpTimeWith<std::vector<ShardType>> ShardingCatalogClientImpl::getAllShards(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern, BSONObj filter) {

    const auto& findRes =
        uassertStatusOK(_exhaustiveFindOnConfig(opCtx,
                                                getConfigReadPreference(opCtx),
                                                readConcern,
                                                NamespaceString::kConfigsvrShardsNamespace,
                                                filter,
                                                BSONObj() /* No sorting */,
                                                boost::none /* No limit */));

    std::vector<ShardType> shards;
    shards.reserve(findRes.value.size());
    for (const BSONObj& doc : findRes.value) {
        auto shardRes = ShardType::fromBSON(doc);
        uassertStatusOKWithContext(shardRes,
                                   str::stream() << "Failed to parse shard document " << doc);

        ShardType& shard = shardRes.getValue();
        uassertStatusOKWithContext(shard.validate(),
                                   str::stream() << "Failed to validate shard document " << doc);
        shards.push_back(std::move(shard));
    }

    return repl::OpTimeWith<std::vector<ShardType>>{std::move(shards), findRes.opTime};
}

Status ShardingCatalogClientImpl::runUserManagementWriteCommand(OperationContext* opCtx,
                                                                StringData commandName,
                                                                const DatabaseName& dbname,
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

            auto isValidUserManagementWriteConcern = visit(
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

    auto swResponse =
        _getConfigShard(opCtx)->runCommand(opCtx,
                                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                           dbname,
                                           cmdToRun,
                                           Milliseconds(defaultConfigCommandTimeoutMS.load()),
                                           Shard::RetryPolicy::kNotIdempotent);

    if (!swResponse.isOK()) {
        return swResponse.getStatus();
    }

    auto response = std::move(swResponse.getValue());

    if (!response.commandStatus.isOK()) {
        if (!response.writeConcernStatus.isOK()) {
            auto wce = getWriteConcernErrorDetailFromBSONObj(response.response);
            invariant(wce);
            return Status{ClusterUMCErrorWithWriteConcernErrorInfo(response.commandStatus, *wce),
                          "Cluster user management command error with write concern error"};
        }
        return response.commandStatus;
    }

    if (!response.writeConcernStatus.isOK()) {
        return response.writeConcernStatus;
    }

    CommandHelpers::filterCommandReplyForPassthrough(response.response, result);
    return Status::OK();
}

bool ShardingCatalogClientImpl::runUserManagementReadCommand(OperationContext* opCtx,
                                                             const DatabaseName& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    auto resultStatus =
        _getConfigShard(opCtx)->runCommand(opCtx,
                                           kConfigPrimaryPreferredReadPreference,
                                           dbname,
                                           cmdObj,
                                           Milliseconds(defaultConfigCommandTimeoutMS.load()),
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

    const auto configShard = _getConfigShard(opCtx);
    for (int retry = 1; retry <= kMaxWriteRetry; retry++) {
        auto response =
            configShard->runBatchWriteCommand(opCtx,
                                              Milliseconds(defaultConfigCommandTimeoutMS.load()),
                                              request,
                                              writeConcern,
                                              Shard::RetryPolicy::kNoRetry);

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
    MONGO_UNREACHABLE_TASSERT(10083534);
}

StatusWith<bool> ShardingCatalogClientImpl::updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    return _updateConfigDocument(opCtx,
                                 nss,
                                 query,
                                 update,
                                 upsert,
                                 writeConcern,
                                 Milliseconds(defaultConfigCommandTimeoutMS.load()));
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

    auto response = _getConfigShard(opCtx)->runBatchWriteCommand(
        opCtx, maxTimeMs, request, writeConcern, Shard::RetryPolicy::kIdempotent);

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
    invariant(nss.isConfigDB());

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

    auto response = _getConfigShard(opCtx)->runBatchWriteCommand(
        opCtx,
        Milliseconds(defaultConfigCommandTimeoutMS.load()),
        request,
        writeConcern,
        Shard::RetryPolicy::kIdempotent);
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

StatusWith<std::vector<KeysCollectionDocument>> ShardingCatalogClientImpl::getNewInternalKeys(
    OperationContext* opCtx,
    StringData purpose,
    const LogicalTime& newerThanThis,
    repl::ReadConcernLevel readConcernLevel) {
    return _getNewKeys<KeysCollectionDocument>(opCtx,
                                               _getConfigShard(opCtx),
                                               NamespaceString::kKeysCollectionNamespace,
                                               purpose,
                                               newerThanThis,
                                               readConcernLevel);
}

StatusWith<std::vector<ExternalKeysCollectionDocument>>
ShardingCatalogClientImpl::getAllExternalKeys(OperationContext* opCtx,
                                              StringData purpose,
                                              repl::ReadConcernLevel readConcernLevel) {
    return _getNewKeys<ExternalKeysCollectionDocument>(
        opCtx,
        _getConfigShard(opCtx),
        NamespaceString::kExternalKeysCollectionNamespace,
        purpose,
        LogicalTime(),
        readConcernLevel);
}

bool ShardingCatalogClientImpl::anyShardRemovedSince(OperationContext* opCtx,
                                                     const Timestamp& clusterTime) {
    auto loggedRemoveShardEvents =
        uassertStatusOK(
            _exhaustiveFindOnConfig(opCtx,
                                    kConfigPrimaryPreferredReadPreference,
                                    repl::ReadConcernLevel::kMajorityReadConcern,
                                    NamespaceString::kConfigsvrShardRemovalLogNamespace,
                                    BSON("_id" << ShardingCatalogClient::kLatestShardRemovalLogId
                                               << RemoveShardEventType::kTimestampFieldName
                                               << BSON("$gte" << clusterTime)),
                                    BSONObj() /*sort*/,
                                    1 /*limit*/))
            .value;
    return !loggedRemoveShardEvents.empty();
}


std::shared_ptr<Shard> ShardingCatalogClientImpl::_getConfigShard(OperationContext* opCtx) {
    if (_overrideConfigShard) {
        return _overrideConfigShard;
    }
    return Grid::get(opCtx)->shardRegistry()->getConfigShard();
}

}  // namespace mongo

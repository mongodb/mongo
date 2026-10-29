// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_config_version_gen.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

ShardingCatalogClientMock::ShardingCatalogClientMock() = default;

ShardingCatalogClientMock::~ShardingCatalogClientMock() = default;

std::vector<BSONObj> ShardingCatalogClientMock::runCatalogAggregation(
    OperationContext* opCtx,
    AggregateCommandRequest& aggRequest,
    const repl::ReadConcernArgs& readConcern,
    const Milliseconds& maxTimeout,
    Shard::RetryPolicy retryPolicy) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

DatabaseType ShardingCatalogClientMock::getDatabase(OperationContext* opCtx,
                                                    const DatabaseName& db,
                                                    repl::ReadConcernArgs readConcern) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

std::vector<DatabaseType> ShardingCatalogClientMock::getAllDBs(
    OperationContext* opCtx,
    repl::ReadConcernArgs readConcern,
    const boost::optional<ReadPreferenceSetting>& readPref) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

CollectionType ShardingCatalogClientMock::getCollection(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        repl::ReadConcernArgs readConcern) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

CollectionType ShardingCatalogClientMock::getCollection(OperationContext* opCtx,
                                                        const UUID& uuid,
                                                        repl::ReadConcernArgs readConcern) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

std::vector<CollectionType> ShardingCatalogClientMock::getShardedCollections(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernArgs readConcern,
    const BSONObj& sort) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

std::vector<CollectionType> ShardingCatalogClientMock::getCollections(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernArgs readConcern,
    const BSONObj& sort) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

std::vector<NamespaceString> ShardingCatalogClientMock::getCollectionNamespacesForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernArgs readConcern,
    const BSONObj& sort) {
    return {};
}

std::vector<NamespaceString> ShardingCatalogClientMock::getShardedCollectionNamespacesForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernArgs readConcern,
    const BSONObj& sort) {
    return {};
}

std::vector<NamespaceString> ShardingCatalogClientMock::getUnsplittableCollectionNamespacesForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernArgs readConcern,
    const BSONObj& sort) {
    return {};
}

StatusWith<std::vector<DatabaseName>> ShardingCatalogClientMock::getDatabasesForShard(
    OperationContext* opCtx, const ShardId& shardName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<std::vector<ChunkType>> ShardingCatalogClientMock::getChunks(
    OperationContext* opCtx,
    const BSONObj& filter,
    const BSONObj& sort,
    boost::optional<int> limit,
    repl::OpTime* opTime,
    const OID& epoch,
    const Timestamp& timestamp,
    repl::ReadConcernArgs readConcern,
    const boost::optional<BSONObj>& hint) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

std::pair<CollectionType, std::vector<ChunkType>> ShardingCatalogClientMock::getCollectionAndChunks(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& sinceVersion,
    const repl::ReadConcernArgs& readConcern) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

StatusWith<std::vector<TagsType>> ShardingCatalogClientMock::getTagsForCollection(
    OperationContext* opCtx, const NamespaceString& nss, boost::optional<long long> limit) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

std::vector<NamespaceString> ShardingCatalogClientMock::getAllNssThatHaveZonesForDatabase(
    OperationContext* opCtx, const DatabaseName& dbName) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

repl::OpTimeWith<std::vector<ShardType>> ShardingCatalogClientMock::getAllShards(
    OperationContext* opCtx, repl::ReadConcernArgs readConcern, BSONObj filter) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

bool ShardingCatalogClientMock::runUserManagementReadCommand(OperationContext* opCtx,
                                                             const DatabaseName& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    return true;
}

StatusWith<BSONObj> ShardingCatalogClientMock::getGlobalSettings(OperationContext* opCtx,
                                                                 std::string_view key) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<VersionType> ShardingCatalogClientMock::getConfigVersion(
    OperationContext* opCtx, repl::ReadConcernArgs readConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::insertConfigDocument(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const BSONObj& doc,
                                                       const WriteConcernOptions& writeConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<bool> ShardingCatalogClientMock::updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    return {updateConfigDocument(opCtx,
                                 nss,
                                 query,
                                 update,
                                 upsert,
                                 writeConcern,
                                 Milliseconds(defaultConfigCommandTimeoutMS.load()))};
}

StatusWith<bool> ShardingCatalogClientMock::updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern,
    Milliseconds maxTimeMs) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::removeConfigDocuments(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        const BSONObj& query,
                                                        const WriteConcernOptions& writeConcern,
                                                        boost::optional<BSONObj> hint) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogClientMock::createDatabase(OperationContext* opCtx,
                                                 std::string_view dbName,
                                                 ShardId primaryShard) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<std::vector<KeysCollectionDocument>> ShardingCatalogClientMock::getNewInternalKeys(
    OperationContext* opCtx,
    std::string_view purpose,
    const LogicalTime& newerThanThis,
    repl::ReadConcernArgs readConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<std::vector<ExternalKeysCollectionDocument>>
ShardingCatalogClientMock::getAllExternalKeys(OperationContext* opCtx,
                                              std::string_view purpose,
                                              repl::ReadConcernArgs readConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<repl::OpTimeWith<std::vector<BSONObj>>>
ShardingCatalogClientMock::_exhaustiveFindOnConfig(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& readPref,
                                                   const repl::ReadConcernArgs& readConcern,
                                                   const NamespaceString& nss,
                                                   const BSONObj& query,
                                                   const BSONObj& sort,
                                                   boost::optional<long long> limit,
                                                   const boost::optional<BSONObj>& hint) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

bool ShardingCatalogClientMock::anyShardRemovedSince(OperationContext* opCtx,
                                                     const Timestamp& clusterTime) {
    return false;
}
}  // namespace mongo

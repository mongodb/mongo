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

#include <boost/move/utility_core.hpp>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/util/assert_util.h"

namespace mongo {

ShardingCatalogClientMock::ShardingCatalogClientMock() = default;

ShardingCatalogClientMock::~ShardingCatalogClientMock() = default;

std::vector<BSONObj> ShardingCatalogClientMock::runCatalogAggregation(
    OperationContext* opCtx,
    AggregateCommandRequest& aggRequest,
    const repl::ReadConcernArgs& readConcern,
    const Milliseconds& maxTimeout) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

DatabaseType ShardingCatalogClientMock::getDatabase(OperationContext* opCtx,
                                                    const DatabaseName& db,
                                                    repl::ReadConcernLevel readConcernLevel) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

std::vector<DatabaseType> ShardingCatalogClientMock::getAllDBs(OperationContext* opCtx,
                                                               repl::ReadConcernLevel readConcern) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

CollectionType ShardingCatalogClientMock::getCollection(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        repl::ReadConcernLevel readConcernLevel) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

CollectionType ShardingCatalogClientMock::getCollection(OperationContext* opCtx,
                                                        const UUID& uuid,
                                                        repl::ReadConcernLevel readConcernLevel) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

std::vector<CollectionType> ShardingCatalogClientMock::getShardedCollections(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernLevel readConcernLevel,
    const BSONObj& sort) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

std::vector<CollectionType> ShardingCatalogClientMock::getCollections(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernLevel readConcernLevel,
    const BSONObj& sort) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

std::vector<NamespaceString> ShardingCatalogClientMock::getCollectionNamespacesForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernLevel readConcern,
    const BSONObj& sort) {
    return {};
}

std::vector<NamespaceString> ShardingCatalogClientMock::getShardedCollectionNamespacesForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernLevel readConcern,
    const BSONObj& sort) {
    return {};
}

std::vector<NamespaceString> ShardingCatalogClientMock::getUnsplittableCollectionNamespacesForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    repl::ReadConcernLevel readConcern,
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
    repl::ReadConcernLevel readConcern,
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

std::pair<CollectionType, std::vector<IndexCatalogType>>
ShardingCatalogClientMock::getCollectionAndShardingIndexCatalogEntries(
    OperationContext* opCtx, const NamespaceString& nss, const repl::ReadConcernArgs& readConcern) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

StatusWith<std::vector<TagsType>> ShardingCatalogClientMock::getTagsForCollection(
    OperationContext* opCtx, const NamespaceString& nss) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

std::vector<NamespaceString> ShardingCatalogClientMock::getAllNssThatHaveZonesForDatabase(
    OperationContext* opCtx, const DatabaseName& dbName) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

StatusWith<repl::OpTimeWith<std::vector<ShardType>>> ShardingCatalogClientMock::getAllShards(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

bool ShardingCatalogClientMock::runUserManagementReadCommand(OperationContext* opCtx,
                                                             const DatabaseName& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    return true;
}

StatusWith<BSONObj> ShardingCatalogClientMock::getGlobalSettings(OperationContext* opCtx,
                                                                 StringData key) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<VersionType> ShardingCatalogClientMock::getConfigVersion(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
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
    return {updateConfigDocument(
        opCtx, nss, query, update, upsert, writeConcern, Shard::kDefaultConfigCommandTimeout)};
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
                                                 StringData dbName,
                                                 ShardId primaryShard) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<std::vector<KeysCollectionDocument>> ShardingCatalogClientMock::getNewInternalKeys(
    OperationContext* opCtx,
    StringData purpose,
    const LogicalTime& newerThanThis,
    repl::ReadConcernLevel readConcernLevel) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<std::vector<ExternalKeysCollectionDocument>>
ShardingCatalogClientMock::getAllExternalKeys(OperationContext* opCtx,
                                              StringData purpose,
                                              repl::ReadConcernLevel readConcernLevel) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

StatusWith<repl::OpTimeWith<std::vector<BSONObj>>>
ShardingCatalogClientMock::_exhaustiveFindOnConfig(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& readPref,
                                                   const repl::ReadConcernLevel& readConcern,
                                                   const NamespaceString& nss,
                                                   const BSONObj& query,
                                                   const BSONObj& sort,
                                                   boost::optional<long long> limit,
                                                   const boost::optional<BSONObj>& hint) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

HistoricalPlacement ShardingCatalogClientMock::getShardsThatOwnDataForCollAtClusterTime(
    OperationContext* opCtx, const NamespaceString& collName, const Timestamp& clusterTime) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

HistoricalPlacement ShardingCatalogClientMock::getShardsThatOwnDataForDbAtClusterTime(
    OperationContext* opCtx, const NamespaceString& dbName, const Timestamp& clusterTime) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

HistoricalPlacement ShardingCatalogClientMock::getShardsThatOwnDataAtClusterTime(
    OperationContext* opCtx, const Timestamp& clusterTime) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}

HistoricalPlacement ShardingCatalogClientMock::getHistoricalPlacement(
    OperationContext* opCtx,
    const Timestamp& atClusterTime,
    const boost::optional<NamespaceString>& nss) {
    uasserted(ErrorCodes::InternalError, "Method not implemented");
}
}  // namespace mongo

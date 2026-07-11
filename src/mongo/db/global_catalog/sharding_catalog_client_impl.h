// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/global_catalog/ddl/placement_history_commands_gen.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class NamespaceString;
struct ReadPreferenceSetting;
class VersionType;

namespace executor {
class TaskExecutor;
}  // namespace executor

/**
 * Builds the aggregation that joins a "collections" namespace with its "chunks" namespace in order
 * to retrieve the routing table of 'nss'.
 *
 * When 'sinceVersion' shares the collection's epoch the aggregation only returns the chunks that
 * changed since that version (incremental refresh). Otherwise it returns all of the collection's
 * chunks (full refresh); pass ChunkVersion::UNTRACKED() to always force a full refresh.
 */
[[MONGO_MOD_PARENT_PRIVATE]] AggregateCommandRequest makeCollectionAndChunksAggregation(
    OperationContext* opCtx,
    const NamespaceString& collectionsNss,
    const NamespaceString& chunksNss,
    const NamespaceString& nss,
    const ChunkVersion& sinceVersion);

/**
 * Implements the catalog client for reading from replica set config servers.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardingCatalogClientImpl final
    : public ShardingCatalogClient {

public:
    ShardingCatalogClientImpl(std::shared_ptr<Shard> overrideConfigShard);
    ~ShardingCatalogClientImpl() override;

    /*
     * Updates (or if "upsert" is true, creates) catalog data for the sharded collection "collNs" by
     * writing a document to the "config.collections" collection with the catalog information
     * described by "coll."
     */
    Status updateShardingCatalogEntryForCollection(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const CollectionType& coll,
                                                   bool upsert);

    std::vector<BSONObj> runCatalogAggregation(
        OperationContext* opCtx,
        AggregateCommandRequest& aggRequest,
        const repl::ReadConcernArgs& readConcern,
        const Milliseconds& maxTimeout = Milliseconds(defaultConfigCommandTimeoutMS.load()),
        Shard::RetryPolicy retryPolicy = Shard::RetryPolicy::kIdempotent) override;

    DatabaseType getDatabase(OperationContext* opCtx,
                             const DatabaseName& db,
                             repl::ReadConcernArgs readConcern) override;

    std::vector<DatabaseType> getAllDBs(
        OperationContext* opCtx,
        repl::ReadConcernArgs readConcern,
        const boost::optional<ReadPreferenceSetting>& readPref = boost::none) override;

    CollectionType getCollection(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 repl::ReadConcernArgs readConcern) override;

    CollectionType getCollection(OperationContext* opCtx,
                                 const UUID& uuid,
                                 repl::ReadConcernArgs readConcern) override;

    std::vector<CollectionType> getShardedCollections(OperationContext* opCtx,
                                                      const DatabaseName& db,
                                                      repl::ReadConcernArgs readConcern,
                                                      const BSONObj& sort) override;

    std::vector<CollectionType> getCollections(OperationContext* opCtx,
                                               const DatabaseName& db,
                                               repl::ReadConcernArgs readConcern,
                                               const BSONObj& sort) override;

    std::vector<NamespaceString> getShardedCollectionNamespacesForDb(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        repl::ReadConcernArgs readConcern,
        const BSONObj& sort = BSONObj()) override;

    std::vector<NamespaceString> getCollectionNamespacesForDb(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        repl::ReadConcernArgs readConcern,
        const BSONObj& sort = BSONObj()) override;

    std::vector<NamespaceString> getUnsplittableCollectionNamespacesForDb(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        repl::ReadConcernArgs readConcern,
        const BSONObj& sort = BSONObj()) override;

    StatusWith<std::vector<DatabaseName>> getDatabasesForShard(OperationContext* opCtx,
                                                               const ShardId& shardName) override;

    StatusWith<std::vector<ChunkType>> getChunks(
        OperationContext* opCtx,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<int> limit,
        repl::OpTime* opTime,
        const OID& epoch,
        const Timestamp& timestamp,
        repl::ReadConcernArgs readConcern,
        const boost::optional<BSONObj>& hint = boost::none) override;

    std::pair<CollectionType, std::vector<ChunkType>> getCollectionAndChunks(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& sinceVersion,
        const repl::ReadConcernArgs& readConcern) override;

    StatusWith<std::vector<TagsType>> getTagsForCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<long long> limit = boost::none) override;

    std::vector<NamespaceString> getAllNssThatHaveZonesForDatabase(
        OperationContext* opCtx, const DatabaseName& dbName) override;

    repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext* opCtx,
                                                          repl::ReadConcernArgs readConcern,
                                                          BSONObj filter = BSONObj()) override;

    Status runUserManagementWriteCommand(OperationContext* opCtx,
                                         std::string_view commandName,
                                         const DatabaseName& dbname,
                                         const BSONObj& cmdObj,
                                         BSONObjBuilder* result) override;

    bool runUserManagementReadCommand(OperationContext* opCtx,
                                      const DatabaseName& dbname,
                                      const BSONObj& cmdObj,
                                      BSONObjBuilder* result) override;

    StatusWith<BSONObj> getGlobalSettings(OperationContext* opCtx, std::string_view key) override;

    StatusWith<VersionType> getConfigVersion(OperationContext* opCtx,
                                             repl::ReadConcernArgs readConcern) override;

    Status insertConfigDocument(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const BSONObj& doc,
                                const WriteConcernOptions& writeConcern) override;

    StatusWith<bool> updateConfigDocument(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& query,
                                          const BSONObj& update,
                                          bool upsert,
                                          const WriteConcernOptions& writeConcern) override;

    StatusWith<bool> updateConfigDocument(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& query,
                                          const BSONObj& update,
                                          bool upsert,
                                          const WriteConcernOptions& writeConcern,
                                          Milliseconds maxTimeMs) override;

    Status removeConfigDocuments(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const BSONObj& query,
                                 const WriteConcernOptions& writeConcern,
                                 boost::optional<BSONObj> hint = boost::none) override;

    StatusWith<std::vector<KeysCollectionDocument>> getNewInternalKeys(
        OperationContext* opCtx,
        std::string_view purpose,
        const LogicalTime& newerThanThis,
        repl::ReadConcernArgs readConcern) override;

    StatusWith<std::vector<ExternalKeysCollectionDocument>> getAllExternalKeys(
        OperationContext* opCtx,
        std::string_view purpose,
        repl::ReadConcernArgs readConcern) override;

    bool anyShardRemovedSince(OperationContext* opCtx, const Timestamp& clusterTime) override;

private:
    /**
     * Updates a single document (if useMultiUpdate is false) or multiple documents (if
     * useMultiUpdate is true) in the specified namespace on the config server. Must only be used
     * for updates to the 'config' database.
     *
     * This method retries the operation on NotPrimary or network errors, so it should only be used
     * with modifications which are idempotent.
     *
     * Returns non-OK status if the command failed to run for some reason. If the command was
     * successful, returns true if a document was actually modified (that is, it did not exist and
     * was upserted or it existed and any of the fields changed) and false otherwise (basically
     * returns whether the update command's response update.n value is > 0).
     */
    StatusWith<bool> _updateConfigDocument(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONObj& query,
                                           const BSONObj& update,
                                           bool upsert,
                                           const WriteConcernOptions& writeConcern,
                                           Milliseconds maxTimeMs);

    StatusWith<repl::OpTimeWith<std::vector<BSONObj>>> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernArgs& readConcern,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint = boost::none) override;

    /**
     * Queries the config servers for the database metadata for the given database, using the
     * given read preference.  Returns NamespaceNotFound if no database metadata is found.
     */
    StatusWith<repl::OpTimeWith<DatabaseType>> _fetchDatabaseMetadata(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const ReadPreferenceSetting& readPref,
        repl::ReadConcernArgs readConcern);

    /**
     * Returns the Shard type that should be used to access the config server. Unless an instance
     * was provided at construction, which may be done e.g. to force using local operations, falls
     * back to using the config shard from the ShardRegistry.
     */
    std::shared_ptr<Shard> _getConfigShard(OperationContext* opCtx);

    // If set, this is used as the config shard by all methods. Be careful to only use an instance
    // that is always valid, like a ShardLocal.
    std::shared_ptr<Shard> _overrideConfigShard;
};

}  // namespace mongo

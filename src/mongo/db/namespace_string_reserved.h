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

#pragma once

// IWYU pragma: private, include "mongo/db/namespace_string.h"

/**
 * This X-macro expands the provided macro as `X(id, db, coll)` for each nss, where:
 *
 * - `id`: the `ConstantProxy` data member of `NamespaceString` being defined.
 * - `db`: must be a constexpr DatabaseName::ConstantProxy expression.
 * - `coll`: must be a constexpr StringData expression.
 */
#define EXPAND_NSS_CONSTANT_TABLE(X)                                                               \
    /*  Namespace for storing configuration data, which needs to be replicated if the server is    \
     *  running as a replica set. Documents in this collection should represent some configuration \
     *  state of the server, which needs to be recovered/consulted at startup. Each document in    \
     *  this namespace should have its _id set to some string, which meaningfully describes what   \
     *  it represents. For example, 'shardIdentity' and 'featureCompatibilityVersion'. */          \
    X(kServerConfigurationNamespace, DatabaseName::kAdmin, "system.version"_sd)                    \
                                                                                                   \
    /*  Namespace for storing the logical sessions information */                                  \
    X(kLogicalSessionsNamespace, DatabaseName::kConfig, "system.sessions"_sd)                      \
                                                                                                   \
    /*  Namespace for storing databases information */                                             \
    X(kConfigDatabasesNamespace, DatabaseName::kConfig, "databases"_sd)                            \
                                                                                                   \
    /*  Namespace for storing the transaction information for each session */                      \
    X(kSessionTransactionsTableNamespace, DatabaseName::kConfig, "transactions"_sd)                \
                                                                                                   \
    /*  Name for a shard's collections metadata collection, each document of which indicates the   \
     *  state of a specific collection */                                                          \
    X(kShardConfigCollectionsNamespace, DatabaseName::kConfig, "cache.collections"_sd)             \
                                                                                                   \
    /*  Name for a shard's non-authoritative databases metadata collection, each document of       \
     * which indicates the state of a specific database. */                                        \
    X(kConfigCacheDatabasesNamespace, DatabaseName::kConfig, "cache.databases"_sd)                 \
                                                                                                   \
    /*  Name for a shard's authoritative databases metadata collection, each document of which     \
     * indicates the state of a specific database. */                                              \
    X(kConfigShardCatalogDatabasesNamespace, DatabaseName::kConfig, "shard.catalog.databases"_sd)  \
                                                                                                   \
    /*  Name for a shard's authoritative collections metadata collection, each document of which   \
     *  indicates the state of a specific collection. */                                           \
    X(kConfigShardCatalogCollectionsNamespace,                                                     \
      DatabaseName::kConfig,                                                                       \
      "shard.catalog.collections"_sd)                                                              \
                                                                                                   \
    /*  Name for a shard's authoritative chunks metadata collection, each document of which        \
     * indicates the state of a specific chunk. */                                                 \
    X(kConfigShardCatalogChunksNamespace, DatabaseName::kConfig, "shard.catalog.chunks"_sd)        \
                                                                                                   \
    /*  Namespace for storing keys for signing and validating cluster times created by             \
     * the cluster that this node is in. */                                                        \
    X(kKeysCollectionNamespace, DatabaseName::kAdmin, "system.keys"_sd)                            \
                                                                                                   \
    /*  Namespace for storing keys for validating cluster times created by other clusters. */      \
    X(kExternalKeysCollectionNamespace, DatabaseName::kConfig, "external_validation_keys"_sd)      \
                                                                                                   \
    /*  Namespace of the the oplog collection. */                                                  \
    X(kRsOplogNamespace, DatabaseName::kLocal, "oplog.rs"_sd)                                      \
                                                                                                   \
    /*  Namespace for storing the persisted state of transaction coordinators. */                  \
    X(kTransactionCoordinatorsNamespace, DatabaseName::kConfig, "transaction_coordinators"_sd)     \
                                                                                                   \
    /*  Namespace for storing the persisted state of migration coordinators. */                    \
    X(kMigrationCoordinatorsNamespace, DatabaseName::kConfig, "migrationCoordinators"_sd)          \
                                                                                                   \
    /*  Namespace for storing the persisted state of migration recipients. */                      \
    X(kMigrationRecipientsNamespace, DatabaseName::kConfig, "migrationRecipients"_sd)              \
                                                                                                   \
    /*  Namespace for replica set configuration settings. */                                       \
    X(kSystemReplSetNamespace, DatabaseName::kLocal, "system.replset"_sd)                          \
                                                                                                   \
    /*  Namespace for storing the last replica set election vote. */                               \
    X(kLastVoteNamespace, DatabaseName::kLocal, "replset.election"_sd)                             \
                                                                                                   \
    /*  Namespace for index build entries. */                                                      \
    X(kIndexBuildEntryNamespace, DatabaseName::kConfig, "system.indexBuilds"_sd)                   \
                                                                                                   \
    /*  Namespace for pending range deletions. */                                                  \
    X(kRangeDeletionNamespace, DatabaseName::kConfig, "rangeDeletions"_sd)                         \
                                                                                                   \
    /*  Namespace containing pending range deletions snapshots for rename operations. */           \
    X(kRangeDeletionForRenameNamespace, DatabaseName::kConfig, "rangeDeletionsForRename"_sd)       \
                                                                                                   \
    /*  Namespace for the coordinator's resharding operation state. */                             \
    X(kConfigReshardingOperationsNamespace, DatabaseName::kConfig, "reshardingOperations"_sd)      \
                                                                                                   \
    /*  Namespace for the donor shard's local resharding operation state. */                       \
    X(kDonorReshardingOperationsNamespace,                                                         \
      DatabaseName::kConfig,                                                                       \
      "localReshardingOperations.donor"_sd)                                                        \
                                                                                                   \
    /*  Namespace for the recipient shard's local resharding operation state. */                   \
    X(kRecipientReshardingOperationsNamespace,                                                     \
      DatabaseName::kConfig,                                                                       \
      "localReshardingOperations.recipient"_sd)                                                    \
                                                                                                   \
    /*  Namespace for the recipient shard's local resharding operation resume data. */             \
    X(kRecipientReshardingResumeDataNamespace,                                                     \
      DatabaseName::kConfig,                                                                       \
      "localReshardingResumeData.recipient"_sd)                                                    \
                                                                                                   \
    /*  Namespace for persisting sharding DDL coordinators state documents */                      \
    X(kShardingDDLCoordinatorsNamespace,                                                           \
      DatabaseName::kConfig,                                                                       \
      "system.sharding_ddl_coordinators"_sd)                                                       \
                                                                                                   \
    /*  Namespace for storing MultiUpdateCoordinator state documents. */                           \
    X(kMultiUpdateCoordinatorsNamespace,                                                           \
      DatabaseName::kConfig,                                                                       \
      "localMigrationBlockingOperations.multiUpdateCoordinators"_sd)                               \
                                                                                                   \
    /*  Namespace for persisting sharding DDL rename participant state documents */                \
    X(kShardingRenameParticipantsNamespace, DatabaseName::kConfig, "localRenameParticipants"_sd)   \
                                                                                                   \
    /*  Namespace for balancer settings and default read and write concerns. */                    \
    X(kConfigSettingsNamespace, DatabaseName::kConfig, "settings"_sd)                              \
                                                                                                   \
    /*  Namespace for vector clock state. */                                                       \
    X(kVectorClockNamespace, DatabaseName::kConfig, "vectorClock"_sd)                              \
                                                                                                   \
    /*  Namespace for storing oplog applier progress for resharding. */                            \
    X(kReshardingApplierProgressNamespace,                                                         \
      DatabaseName::kConfig,                                                                       \
      "localReshardingOperations.recipient.progress_applier"_sd)                                   \
                                                                                                   \
    /*  Namespace for storing oplog fetcher progress for resharding. */                            \
    X(kReshardingFetcherProgressNamespace,                                                         \
      DatabaseName::kConfig,                                                                       \
      "localReshardingOperations.recipient.progress_fetcher"_sd)                                   \
                                                                                                   \
    /*  Namespace for storing config.transactions cloner progress for resharding. */               \
    X(kReshardingTxnClonerProgressNamespace,                                                       \
      DatabaseName::kConfig,                                                                       \
      "localReshardingOperations.recipient.progress_txn_cloner"_sd)                                \
                                                                                                   \
    /*  Namespace for serializing between dropDatabase and a following concurrent                  \
     * createDatabase. The dropDatabase DDL coordinator will be the only writer to this            \
     * collection. */                                                                              \
    X(kConfigDropPendingDBsNamespace, DatabaseName::kConfig, "dropPendingDBs"_sd)                  \
                                                                                                   \
    /*  Namespace for storing config.collectionCriticalSections documents */                       \
    X(kCollectionCriticalSectionsNamespace,                                                        \
      DatabaseName::kConfig,                                                                       \
      "collection_critical_sections"_sd)                                                           \
                                                                                                   \
    /*  Dummy namespace used for forcing secondaries to handle an oplog entry on its own batch. */ \
    X(kForceOplogBatchBoundaryNamespace,                                                           \
      DatabaseName::kConfig,                                                                       \
      "system.forceOplogBatchBoundary"_sd)                                                         \
                                                                                                   \
    /*  Namespace used for storing retryable findAndModify images. */                              \
    X(kConfigImagesNamespace, DatabaseName::kConfig, "image_collection"_sd)                        \
                                                                                                   \
    /*  Namespace used for persisting ConfigsvrCoordinator state documents. */                     \
    X(kConfigsvrCoordinatorsNamespace,                                                             \
      DatabaseName::kConfig,                                                                       \
      "sharding_configsvr_coordinators"_sd)                                                        \
                                                                                                   \
    /*  Namespace for storing user write blocking critical section documents */                    \
    X(kUserWritesCriticalSectionsNamespace,                                                        \
      DatabaseName::kConfig,                                                                       \
      "user_writes_critical_sections"_sd)                                                          \
                                                                                                   \
    /*  Namespace used during the recovery procedure for the config server. */                     \
    X(kConfigsvrRestoreNamespace, DatabaseName::kLocal, "system.collections_to_restore"_sd)        \
                                                                                                   \
    /*  Namespace used for CompactParticipantCoordinator service. */                               \
    X(kCompactStructuredEncryptionCoordinatorNamespace,                                            \
      DatabaseName::kConfig,                                                                       \
      "compact_structured_encryption_coordinator"_sd)                                              \
                                                                                                   \
    /*  Namespace used for storing cluster wide parameters on dedicated configurations. */         \
    X(kClusterParametersNamespace, DatabaseName::kConfig, "clusterParameters"_sd)                  \
                                                                                                   \
    /*  Namespace used for storing the list of shards on the CSRS. */                              \
    X(kConfigsvrShardsNamespace, DatabaseName::kConfig, "shards"_sd)                               \
                                                                                                   \
    /*  Namespace used for storing the list of sharded collections on the CSRS. */                 \
    X(kConfigsvrCollectionsNamespace, DatabaseName::kConfig, "collections"_sd)                     \
                                                                                                   \
    /*  Namespace used for storing the index catalog on the CSRS. */                               \
    X(kConfigsvrIndexCatalogNamespace, DatabaseName::kConfig, "csrs.indexes"_sd)                   \
                                                                                                   \
    /*  Namespace used for storing the index catalog on the shards. */                             \
    X(kShardIndexCatalogNamespace, DatabaseName::kConfig, "shard.indexes"_sd)                      \
                                                                                                   \
    /*  Namespace used for storing the collection catalog on the shards. */                        \
    X(kShardCollectionCatalogNamespace, DatabaseName::kConfig, "shard.collections"_sd)             \
                                                                                                   \
    /*  Namespace used for storing NamespacePlacementType docs on the CSRS. */                     \
    X(kConfigsvrPlacementHistoryNamespace, DatabaseName::kConfig, "placementHistory"_sd)           \
                                                                                                   \
    /*  Namespace used for storing a single document with the timestamp of the latest              \
     * removeShard committed on the CSRS. */                                                       \
    X(kConfigsvrShardRemovalLogNamespace, DatabaseName::kConfig, "shardRemovalLog"_sd)             \
                                                                                                   \
    /*  Namespace used to store the state document of 'SetChangeStreamStateCoordinator'. */        \
    X(kSetChangeStreamStateCoordinatorNamespace,                                                   \
      DatabaseName::kConfig,                                                                       \
      "change_stream_coordinator"_sd)                                                              \
                                                                                                   \
    /*  Namespace used to store change stream pre-images */                                        \
    X(kChangeStreamPreImagesNamespace, DatabaseName::kConfig, "system.preimages"_sd)               \
                                                                                                   \
    /*  Namespace used by an analyzeShardKey command to store the split points for the shard       \
     *  being analyzed. */                                                                         \
    X(kConfigAnalyzeShardKeySplitPointsNamespace,                                                  \
      DatabaseName::kConfig,                                                                       \
      "analyzeShardKeySplitPoints"_sd)                                                             \
                                                                                                   \
    /*  Namespace used for storing query analyzer settings. */                                     \
    X(kConfigQueryAnalyzersNamespace, DatabaseName::kConfig, "queryAnalyzers"_sd)                  \
                                                                                                   \
    /*  Namespace used for storing sampled queries. */                                             \
    X(kConfigSampledQueriesNamespace, DatabaseName::kConfig, "sampledQueries"_sd)                  \
                                                                                                   \
    /*  Namespace used for storing the diffs for sampled update queries. */                        \
    X(kConfigSampledQueriesDiffNamespace, DatabaseName::kConfig, "sampledQueriesDiff"_sd)          \
                                                                                                   \
    /*  Namespace used for storing query shape representative queries. */                          \
    X(kQueryShapeRepresentativeQueriesNamespace,                                                   \
      DatabaseName::kConfig,                                                                       \
      "queryShapeRepresentativeQueries"_sd)                                                        \
                                                                                                   \
    /*  Namespace used for the health log. */                                                      \
    X(kLocalHealthLogNamespace, DatabaseName::kLocal, "system.healthlog"_sd)                       \
                                                                                                   \
    /*  Namespace used for command oplog entries. */                                               \
    X(kAdminCommandNamespace, DatabaseName::kAdmin, "$cmd"_sd)                                     \
                                                                                                   \
    /*  Namespace used to store roles. */                                                          \
    X(kAdminRolesNamespace, DatabaseName::kAdmin, "system.roles"_sd)                               \
                                                                                                   \
    /*  Namespace used to store users. */                                                          \
    X(kAdminUsersNamespace, DatabaseName::kAdmin, "system.users"_sd)                               \
                                                                                                   \
    /*  Namespace used by mms-automation. */                                                       \
    X(kLocalClusterManagerNamespace, DatabaseName::kLocal, "clustermanager"_sd)                    \
                                                                                                   \
    /*  Namespace used for startup log. */                                                         \
    X(kStartupLogNamespace, DatabaseName::kLocal, "startup_log"_sd)                                \
                                                                                                   \
    /*  Namespace for changelog on CSRS. */                                                        \
    X(kConfigChangelogNamespace, DatabaseName::kConfig, "changelog"_sd)                            \
                                                                                                   \
    /*  Namespace used for storing the list of chunks on the CSRS. */                              \
    X(kConfigsvrChunksNamespace, DatabaseName::kConfig, "chunks"_sd)                               \
                                                                                                   \
    /*  Namespace used for storing the list of tags on the CSRS. */                                \
    X(kConfigsvrTagsNamespace, DatabaseName::kConfig, "tags"_sd)                                   \
                                                                                                   \
    /*  Namespace used for storing version info on the CSRS. */                                    \
    X(kConfigVersionNamespace, DatabaseName::kConfig, "version"_sd)                                \
                                                                                                   \
    /*  Namespace used for storing mongos info on the CSRS. */                                     \
    X(kConfigMongosNamespace, DatabaseName::kConfig, "mongos"_sd)                                  \
                                                                                                   \
    /*  Namespace used for oplog truncate after point. */                                          \
    X(kDefaultOplogTruncateAfterPointNamespace,                                                    \
      DatabaseName::kLocal,                                                                        \
      "replset.oplogTruncateAfterPoint"_sd)                                                        \
                                                                                                   \
    /*  Namespace used for local system rollback id. */                                            \
    X(kDefaultRollbackIdNamespace, DatabaseName::kLocal, "system.rollback.id"_sd)                  \
                                                                                                   \
    /*  Namespace used for the local oplog dollar main namespace. */                               \
    X(kLocalOplogDollarMain, DatabaseName::kLocal, "oplog.$main"_sd)                               \
                                                                                                   \
    /*  Namespace used for local replset initial sync id. */                                       \
    X(kDefaultInitialSyncIdNamespace, DatabaseName::kLocal, "replset.initialSyncId"_sd)            \
                                                                                                   \
    /*  Namespace used for local temporary oplog buffer. */                                        \
    X(kDefaultOplogCollectionNamespace, DatabaseName::kLocal, "temp_oplog_buffer"_sd)              \
                                                                                                   \
    /*  Namespace used for local minimum valid namespace. */                                       \
    X(kDefaultMinValidNamespace, DatabaseName::kLocal, "replset.minvalid"_sd)                      \
                                                                                                   \
    /*  Namespace used by the test command to pin the oldest timestamp. */                         \
    X(kDurableHistoryTestNamespace, DatabaseName::kMdbTesting, "pinned_timestamp"_sd)              \
                                                                                                   \
    /*  Namespace used by DocumentSourceOut on shard servers to store a list of temporary          \
     * collections that shall be garbage-collected (dropped) on the next step up. */               \
    X(kAggTempCollections, DatabaseName::kConfig, "agg_temp_collections"_sd)                       \
                                                                                                   \
    X(kEmpty, DatabaseName::kEmpty, ""_sd)                                                         \
                                                                                                   \
    /*  Namespace for storing feature compatibility version changes block documents */             \
    X(kBlockFCVChangesNamespace, DatabaseName::kConfig, "system.block_fcv_changes"_sd)             \
    /**/

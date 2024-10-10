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

/**
 * This file is included multiple times into `namespace_string.h`, in order to
 * define NamespaceString constexpr values. The `NSS_CONSTANT` macro callback
 * will be defined differently at each include. Lines here are of the form:
 *
 *     NSS_CONSTANT(id, db, coll)
 *
 * - `id` is the `ConstantProxy` data member of `NamespaceString` being defined.
 * - `db` : must be a constexpr DatabaseName::ConstantProxy expression.
 * - `coll` must be a constexpr StringData expression.
 */

// Namespace for storing configuration data, which needs to be replicated if the server is
// running as a replica set. Documents in this collection should represent some configuration
// state of the server, which needs to be recovered/consulted at startup. Each document in this
// namespace should have its _id set to some string, which meaningfully describes what it
// represents. For example, 'shardIdentity' and 'featureCompatibilityVersion'.
NSS_CONSTANT(kServerConfigurationNamespace, DatabaseName::kAdmin, "system.version"_sd)

// Namespace for storing the logical sessions information
NSS_CONSTANT(kLogicalSessionsNamespace, DatabaseName::kConfig, "system.sessions"_sd)

// Namespace for storing databases information
NSS_CONSTANT(kConfigDatabasesNamespace, DatabaseName::kConfig, "databases"_sd)

// Namespace for storing the transaction information for each session
NSS_CONSTANT(kSessionTransactionsTableNamespace, DatabaseName::kConfig, "transactions"_sd)

// Name for a shard's collections metadata collection, each document of which indicates the
// state of a specific collection
NSS_CONSTANT(kShardConfigCollectionsNamespace, DatabaseName::kConfig, "cache.collections"_sd)

// Name for a shard's databases metadata collection, each document of which indicates the state
// of a specific database
NSS_CONSTANT(kShardConfigDatabasesNamespace, DatabaseName::kConfig, "cache.databases"_sd)

// Namespace for storing keys for signing and validating cluster times created by the cluster
// that this node is in.
NSS_CONSTANT(kKeysCollectionNamespace, DatabaseName::kAdmin, "system.keys"_sd)

// Namespace for storing keys for validating cluster times created by other clusters.
NSS_CONSTANT(kExternalKeysCollectionNamespace, DatabaseName::kConfig, "external_validation_keys"_sd)

// Namespace of the the oplog collection.
NSS_CONSTANT(kRsOplogNamespace, DatabaseName::kLocal, "oplog.rs"_sd)

// Namespace for storing the persisted state of transaction coordinators.
NSS_CONSTANT(kTransactionCoordinatorsNamespace,
             DatabaseName::kConfig,
             "transaction_coordinators"_sd)

// Namespace for storing the persisted state of migration coordinators.
NSS_CONSTANT(kMigrationCoordinatorsNamespace, DatabaseName::kConfig, "migrationCoordinators"_sd)

// Namespace for storing the persisted state of migration recipients.
NSS_CONSTANT(kMigrationRecipientsNamespace, DatabaseName::kConfig, "migrationRecipients"_sd)

// Namespace for storing the persisted state of tenant migration donors.
NSS_CONSTANT(kTenantMigrationDonorsNamespace, DatabaseName::kConfig, "tenantMigrationDonors"_sd)

// Namespace for storing the persisted state of tenant migration recipient service instances.
NSS_CONSTANT(kTenantMigrationRecipientsNamespace,
             DatabaseName::kConfig,
             "tenantMigrationRecipients"_sd)

// Namespace for view on local.oplog.rs for tenant migrations.
NSS_CONSTANT(kTenantMigrationOplogView, DatabaseName::kLocal, "system.tenantMigration.oplogView"_sd)

// Namespace for replica set configuration settings.
NSS_CONSTANT(kSystemReplSetNamespace, DatabaseName::kLocal, "system.replset"_sd)

// Namespace for storing the last replica set election vote.
NSS_CONSTANT(kLastVoteNamespace, DatabaseName::kLocal, "replset.election"_sd)

// Namespace for index build entries.
NSS_CONSTANT(kIndexBuildEntryNamespace, DatabaseName::kConfig, "system.indexBuilds"_sd)

// Namespace for pending range deletions.
NSS_CONSTANT(kRangeDeletionNamespace, DatabaseName::kConfig, "rangeDeletions"_sd)

// Namespace containing pending range deletions snapshots for rename operations.
NSS_CONSTANT(kRangeDeletionForRenameNamespace, DatabaseName::kConfig, "rangeDeletionsForRename"_sd)

// Namespace for the coordinator's resharding operation state.
NSS_CONSTANT(kConfigReshardingOperationsNamespace, DatabaseName::kConfig, "reshardingOperations"_sd)

// Namespace for the donor shard's local resharding operation state.
NSS_CONSTANT(kDonorReshardingOperationsNamespace,
             DatabaseName::kConfig,
             "localReshardingOperations.donor"_sd)

// Namespace for the recipient shard's local resharding operation state.
NSS_CONSTANT(kRecipientReshardingOperationsNamespace,
             DatabaseName::kConfig,
             "localReshardingOperations.recipient"_sd)

// Namespace for the recipient shard's local resharding operation resume data.
NSS_CONSTANT(kRecipientReshardingResumeDataNamespace,
             DatabaseName::kConfig,
             "localReshardingResumeData.recipient"_sd)

// Namespace for persisting sharding DDL coordinators state documents
NSS_CONSTANT(kShardingDDLCoordinatorsNamespace,
             DatabaseName::kConfig,
             "system.sharding_ddl_coordinators"_sd)

// Namespace for storing MultiUpdateCoordinator state documents.
NSS_CONSTANT(kMultiUpdateCoordinatorsNamespace,
             DatabaseName::kConfig,
             "localMigrationBlockingOperations.multiUpdateCoordinators"_sd)

// Namespace for persisting sharding DDL rename participant state documents
NSS_CONSTANT(kShardingRenameParticipantsNamespace,
             DatabaseName::kConfig,
             "localRenameParticipants"_sd)

// Namespace for balancer settings and default read and write concerns.
NSS_CONSTANT(kConfigSettingsNamespace, DatabaseName::kConfig, "settings"_sd)

// Namespace for vector clock state.
NSS_CONSTANT(kVectorClockNamespace, DatabaseName::kConfig, "vectorClock"_sd)

// Namespace for storing oplog applier progress for resharding.
NSS_CONSTANT(kReshardingApplierProgressNamespace,
             DatabaseName::kConfig,
             "localReshardingOperations.recipient.progress_applier"_sd)

// Namespace for storing config.transactions cloner progress for resharding.
NSS_CONSTANT(kReshardingTxnClonerProgressNamespace,
             DatabaseName::kConfig,
             "localReshardingOperations.recipient.progress_txn_cloner"_sd)

// Namespace for storing config.collectionCriticalSections documents
NSS_CONSTANT(kCollectionCriticalSectionsNamespace,
             DatabaseName::kConfig,
             "collection_critical_sections"_sd)

// Dummy namespace used for forcing secondaries to handle an oplog entry on its own batch.
NSS_CONSTANT(kForceOplogBatchBoundaryNamespace,
             DatabaseName::kConfig,
             "system.forceOplogBatchBoundary"_sd)

// Namespace used for storing retryable findAndModify images.
NSS_CONSTANT(kConfigImagesNamespace, DatabaseName::kConfig, "image_collection"_sd)

// Namespace used for persisting ConfigsvrCoordinator state documents.
NSS_CONSTANT(kConfigsvrCoordinatorsNamespace,
             DatabaseName::kConfig,
             "sharding_configsvr_coordinators"_sd)

// Namespace for storing user write blocking critical section documents
NSS_CONSTANT(kUserWritesCriticalSectionsNamespace,
             DatabaseName::kConfig,
             "user_writes_critical_sections"_sd)

// Namespace used during the recovery procedure for the config server.
NSS_CONSTANT(kConfigsvrRestoreNamespace, DatabaseName::kLocal, "system.collections_to_restore"_sd)

// Namespace used for CompactParticipantCoordinator service.
NSS_CONSTANT(kCompactStructuredEncryptionCoordinatorNamespace,
             DatabaseName::kConfig,
             "compact_structured_encryption_coordinator"_sd)

// Namespace used for storing cluster wide parameters on dedicated configurations.
NSS_CONSTANT(kClusterParametersNamespace, DatabaseName::kConfig, "clusterParameters"_sd)

// Namespace used for storing the list of shards on the CSRS.
NSS_CONSTANT(kConfigsvrShardsNamespace, DatabaseName::kConfig, "shards"_sd)

// Namespace used for storing the list of sharded collections on the CSRS.
NSS_CONSTANT(kConfigsvrCollectionsNamespace, DatabaseName::kConfig, "collections"_sd)

// Namespace used for storing the index catalog on the CSRS.
NSS_CONSTANT(kConfigsvrIndexCatalogNamespace, DatabaseName::kConfig, "csrs.indexes"_sd)

// Namespace used for storing the index catalog on the shards.
NSS_CONSTANT(kShardIndexCatalogNamespace, DatabaseName::kConfig, "shard.indexes"_sd)

// Namespace used for storing the collection catalog on the shards.
NSS_CONSTANT(kShardCollectionCatalogNamespace, DatabaseName::kConfig, "shard.collections"_sd)

// Namespace used for storing NamespacePlacementType docs on the CSRS.
NSS_CONSTANT(kConfigsvrPlacementHistoryNamespace, DatabaseName::kConfig, "placementHistory"_sd)

// Namespace used for storing a single document with the timestamp of the latest removeShard
// committed on the CSRS.
NSS_CONSTANT(kConfigsvrShardRemovalLogNamespace, DatabaseName::kConfig, "shardRemovalLog"_sd)

// Namespace used to store the state document of 'SetChangeStreamStateCoordinator'.
NSS_CONSTANT(kSetChangeStreamStateCoordinatorNamespace,
             DatabaseName::kConfig,
             "change_stream_coordinator"_sd)

// Namespace used by an analyzeShardKey command to store the split points for the shard key being
// analyzed.
NSS_CONSTANT(kConfigAnalyzeShardKeySplitPointsNamespace,
             DatabaseName::kConfig,
             "analyzeShardKeySplitPoints"_sd)

// Namespace used for storing query analyzer settings.
NSS_CONSTANT(kConfigQueryAnalyzersNamespace, DatabaseName::kConfig, "queryAnalyzers"_sd)

// Namespace used for storing sampled queries.
NSS_CONSTANT(kConfigSampledQueriesNamespace, DatabaseName::kConfig, "sampledQueries"_sd)

// Namespace used for storing the diffs for sampled update queries.
NSS_CONSTANT(kConfigSampledQueriesDiffNamespace, DatabaseName::kConfig, "sampledQueriesDiff"_sd)

// Namespace used for the health log.
NSS_CONSTANT(kLocalHealthLogNamespace, DatabaseName::kLocal, "system.healthlog"_sd)

// Namespace used for command oplog entries.
NSS_CONSTANT(kAdminCommandNamespace, DatabaseName::kAdmin, "$cmd"_sd)

// Namespace used to store roles.
NSS_CONSTANT(kAdminRolesNamespace, DatabaseName::kAdmin, "system.roles"_sd)

// Namespace used to store users.
NSS_CONSTANT(kAdminUsersNamespace, DatabaseName::kAdmin, "system.users"_sd)

// Namespace used by mms-automation.
NSS_CONSTANT(kLocalClusterManagerNamespace, DatabaseName::kLocal, "clustermanager"_sd)

// Namespace used for startup log.
NSS_CONSTANT(kStartupLogNamespace, DatabaseName::kLocal, "startup_log"_sd)

// Namespace for changelog on CSRS.
NSS_CONSTANT(kConfigChangelogNamespace, DatabaseName::kConfig, "changelog"_sd)

// Namespace used for storing the list of chunks on the CSRS.
NSS_CONSTANT(kConfigsvrChunksNamespace, DatabaseName::kConfig, "chunks"_sd)

// Namespace used for storing the list of tags on the CSRS.
NSS_CONSTANT(kConfigsvrTagsNamespace, DatabaseName::kConfig, "tags"_sd)

// Namespace used for storing version info on the CSRS.
NSS_CONSTANT(kConfigVersionNamespace, DatabaseName::kConfig, "version"_sd)

// Namespace used for storing mongos info on the CSRS.
NSS_CONSTANT(kConfigMongosNamespace, DatabaseName::kConfig, "mongos"_sd)

// Namespace used for oplog truncate after point.
NSS_CONSTANT(kDefaultOplogTruncateAfterPointNamespace,
             DatabaseName::kLocal,
             "replset.oplogTruncateAfterPoint"_sd)

// Namespace used for local system rollback id.
NSS_CONSTANT(kDefaultRollbackIdNamespace, DatabaseName::kLocal, "system.rollback.id"_sd)

// Namespace used for the local oplog dollar main namespace.
NSS_CONSTANT(kLocalOplogDollarMain, DatabaseName::kLocal, "oplog.$main"_sd)

// Namespace used for local replset initial sync id.
NSS_CONSTANT(kDefaultInitialSyncIdNamespace, DatabaseName::kLocal, "replset.initialSyncId"_sd)

// Namespace used for local temporary oplog buffer.
NSS_CONSTANT(kDefaultOplogCollectionNamespace, DatabaseName::kLocal, "temp_oplog_buffer"_sd)

// Namespace used for local minimum valid namespace.
NSS_CONSTANT(kDefaultMinValidNamespace, DatabaseName::kLocal, "replset.minvalid"_sd)

// Namespace used by the test command to pin the oldest timestamp.
NSS_CONSTANT(kDurableHistoryTestNamespace, DatabaseName::kMdbTesting, "pinned_timestamp"_sd)

// Namespace used by DocumentSourceOut on shard servers to store a list of temporary collections
// that shall be garbage-collected (dropped) on the next step up.
NSS_CONSTANT(kAggTempCollections, DatabaseName::kConfig, "agg_temp_collections"_sd)

NSS_CONSTANT(kEmpty, DatabaseName::kEmpty, ""_sd)

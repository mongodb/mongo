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
 * - `db` : must be a constexpr StringData expression.
 * - `coll` must be a constexpr StringData expression.
 */

// Namespace for storing configuration data, which needs to be replicated if the server is
// running as a replica set. Documents in this collection should represent some configuration
// state of the server, which needs to be recovered/consulted at startup. Each document in this
// namespace should have its _id set to some string, which meaningfully describes what it
// represents. For example, 'shardIdentity' and 'featureCompatibilityVersion'.
NSS_CONSTANT(kServerConfigurationNamespace, NamespaceString::kAdminDb, "system.version"_sd)

// Namespace for storing the logical sessions information
NSS_CONSTANT(kLogicalSessionsNamespace, NamespaceString::kConfigDb, "system.sessions"_sd)

// Namespace for storing databases information
NSS_CONSTANT(kConfigDatabasesNamespace, NamespaceString::kConfigDb, "databases"_sd)

// Namespace for storing the transaction information for each session
NSS_CONSTANT(kSessionTransactionsTableNamespace, NamespaceString::kConfigDb, "transactions"_sd)

// Name for a shard's collections metadata collection, each document of which indicates the
// state of a specific collection
NSS_CONSTANT(kShardConfigCollectionsNamespace, NamespaceString::kConfigDb, "cache.collections"_sd)

// Name for a shard's databases metadata collection, each document of which indicates the state
// of a specific database
NSS_CONSTANT(kShardConfigDatabasesNamespace, NamespaceString::kConfigDb, "cache.databases"_sd)

// Namespace for storing keys for signing and validating cluster times created by the cluster
// that this node is in.
NSS_CONSTANT(kKeysCollectionNamespace, NamespaceString::kAdminDb, "system.keys"_sd)

// Namespace for storing keys for validating cluster times created by other clusters.
NSS_CONSTANT(kExternalKeysCollectionNamespace,
             NamespaceString::kConfigDb,
             "external_validation_keys"_sd)

// Namespace of the the oplog collection.
NSS_CONSTANT(kRsOplogNamespace, NamespaceString::kLocalDb, "oplog.rs"_sd)

// Namespace for storing the persisted state of transaction coordinators.
NSS_CONSTANT(kTransactionCoordinatorsNamespace,
             NamespaceString::kConfigDb,
             "transaction_coordinators"_sd)

// Namespace for storing the persisted state of migration coordinators.
NSS_CONSTANT(kMigrationCoordinatorsNamespace,
             NamespaceString::kConfigDb,
             "migrationCoordinators"_sd)

// Namespace for storing the persisted state of migration recipients.
NSS_CONSTANT(kMigrationRecipientsNamespace, NamespaceString::kConfigDb, "migrationRecipients"_sd)

// Namespace for storing the persisted state of movePrimary operation recipients.
NSS_CONSTANT(kMovePrimaryRecipientNamespace, NamespaceString::kConfigDb, "movePrimaryRecipients"_sd)

// Namespace for storing the persisted state of tenant migration donors.
NSS_CONSTANT(kTenantMigrationDonorsNamespace,
             NamespaceString::kConfigDb,
             "tenantMigrationDonors"_sd)

// Namespace for storing the persisted state of tenant migration recipient service instances.
NSS_CONSTANT(kTenantMigrationRecipientsNamespace,
             NamespaceString::kConfigDb,
             "tenantMigrationRecipients"_sd)

// Namespace for view on local.oplog.rs for tenant migrations.
NSS_CONSTANT(kTenantMigrationOplogView,
             NamespaceString::kLocalDb,
             "system.tenantMigration.oplogView"_sd)

// Namespace for storing the persisted state of tenant split donors.
NSS_CONSTANT(kShardSplitDonorsNamespace, NamespaceString::kConfigDb, "shardSplitDonors"_sd)

// Namespace for replica set configuration settings.
NSS_CONSTANT(kSystemReplSetNamespace, NamespaceString::kLocalDb, "system.replset"_sd)

// Namespace for storing the last replica set election vote.
NSS_CONSTANT(kLastVoteNamespace, NamespaceString::kLocalDb, "replset.election"_sd)

// Namespace for index build entries.
NSS_CONSTANT(kIndexBuildEntryNamespace, NamespaceString::kConfigDb, "system.indexBuilds"_sd)

// Namespace for pending range deletions.
NSS_CONSTANT(kRangeDeletionNamespace, NamespaceString::kConfigDb, "rangeDeletions"_sd)

// Namespace containing pending range deletions snapshots for rename operations.
NSS_CONSTANT(kRangeDeletionForRenameNamespace,
             NamespaceString::kConfigDb,
             "rangeDeletionsForRename"_sd)

// Namespace for the coordinator's resharding operation state.
NSS_CONSTANT(kConfigReshardingOperationsNamespace,
             NamespaceString::kConfigDb,
             "reshardingOperations"_sd)

// Namespace for the donor shard's local resharding operation state.
NSS_CONSTANT(kDonorReshardingOperationsNamespace,
             NamespaceString::kConfigDb,
             "localReshardingOperations.donor"_sd)

// Namespace for the recipient shard's local resharding operation state.
NSS_CONSTANT(kRecipientReshardingOperationsNamespace,
             NamespaceString::kConfigDb,
             "localReshardingOperations.recipient"_sd)

// Namespace for persisting sharding DDL coordinators state documents
NSS_CONSTANT(kShardingDDLCoordinatorsNamespace,
             NamespaceString::kConfigDb,
             "system.sharding_ddl_coordinators"_sd)

// Namespace for persisting sharding DDL rename participant state documents
NSS_CONSTANT(kShardingRenameParticipantsNamespace,
             NamespaceString::kConfigDb,
             "localRenameParticipants"_sd)

// Namespace for balancer settings and default read and write concerns.
NSS_CONSTANT(kConfigSettingsNamespace, NamespaceString::kConfigDb, "settings"_sd)

// Namespace for vector clock state.
NSS_CONSTANT(kVectorClockNamespace, NamespaceString::kConfigDb, "vectorClock"_sd)

// Namespace for storing oplog applier progress for resharding.
NSS_CONSTANT(kReshardingApplierProgressNamespace,
             NamespaceString::kConfigDb,
             "localReshardingOperations.recipient.progress_applier"_sd)

// Namespace for storing config.transactions cloner progress for resharding.
NSS_CONSTANT(kReshardingTxnClonerProgressNamespace,
             NamespaceString::kConfigDb,
             "localReshardingOperations.recipient.progress_txn_cloner"_sd)

// Namespace for storing config.collectionCriticalSections documents
NSS_CONSTANT(kCollectionCriticalSectionsNamespace,
             NamespaceString::kConfigDb,
             "collection_critical_sections"_sd)

// Dummy namespace used for forcing secondaries to handle an oplog entry on its own batch.
NSS_CONSTANT(kForceOplogBatchBoundaryNamespace,
             NamespaceString::kConfigDb,
             "system.forceOplogBatchBoundary"_sd)

// Namespace used for storing retryable findAndModify images.
NSS_CONSTANT(kConfigImagesNamespace, NamespaceString::kConfigDb, "image_collection"_sd)

// Namespace used for persisting ConfigsvrCoordinator state documents.
NSS_CONSTANT(kConfigsvrCoordinatorsNamespace,
             NamespaceString::kConfigDb,
             "sharding_configsvr_coordinators"_sd)

// Namespace for storing user write blocking critical section documents
NSS_CONSTANT(kUserWritesCriticalSectionsNamespace,
             NamespaceString::kConfigDb,
             "user_writes_critical_sections"_sd)

// Namespace used during the recovery procedure for the config server.
NSS_CONSTANT(kConfigsvrRestoreNamespace,
             NamespaceString::kLocalDb,
             "system.collections_to_restore"_sd)

// Namespace used for CompactParticipantCoordinator service.
NSS_CONSTANT(kCompactStructuredEncryptionCoordinatorNamespace,
             NamespaceString::kConfigDb,
             "compact_structured_encryption_coordinator"_sd)

// Namespace used for storing cluster wide parameters on dedicated configurations.
NSS_CONSTANT(kClusterParametersNamespace, NamespaceString::kConfigDb, "clusterParameters"_sd)

// Namespace used for storing the list of shards on the CSRS.
NSS_CONSTANT(kConfigsvrShardsNamespace, NamespaceString::kConfigDb, "shards"_sd)

// Namespace used for storing the list of sharded collections on the CSRS.
NSS_CONSTANT(kConfigsvrCollectionsNamespace, NamespaceString::kConfigDb, "collections"_sd)

// Namespace used for storing the index catalog on the CSRS.
NSS_CONSTANT(kConfigsvrIndexCatalogNamespace, NamespaceString::kConfigDb, "csrs.indexes"_sd)

// Namespace used for storing the index catalog on the shards.
NSS_CONSTANT(kShardIndexCatalogNamespace, NamespaceString::kConfigDb, "shard.indexes"_sd)

// Namespace used for storing the collection catalog on the shards.
NSS_CONSTANT(kShardCollectionCatalogNamespace, NamespaceString::kConfigDb, "shard.collections"_sd)

// Namespace used for storing NamespacePlacementType docs on the CSRS.
NSS_CONSTANT(kConfigsvrPlacementHistoryNamespace, NamespaceString::kConfigDb, "placementHistory"_sd)

// Namespace value used to identify the "fcv marker entry" of
// kConfigsvrPlacementHistoryNamespace collection which marks the start or the end of a FCV
// upgrade/downgrade.
NSS_CONSTANT(kConfigsvrPlacementHistoryFcvMarkerNamespace, StringData{}, StringData{})

// TODO SERVER-68551: remove once 7.0 becomes last-lts
NSS_CONSTANT(kLockpingsNamespace, NamespaceString::kConfigDb, "lockpings"_sd)

// TODO SERVER-68551: remove once 7.0 becomes last-lts
NSS_CONSTANT(kDistLocksNamepsace, NamespaceString::kConfigDb, "locks"_sd)

// Namespace used to store the state document of 'SetChangeStreamStateCoordinator'.
NSS_CONSTANT(kSetChangeStreamStateCoordinatorNamespace,
             NamespaceString::kConfigDb,
             "change_stream_coordinator"_sd)

// Namespace used for storing global index cloner state documents.
NSS_CONSTANT(kGlobalIndexClonerNamespace,
             NamespaceString::kConfigDb,
             "localGlobalIndexOperations.cloner"_sd)

// Namespace used for storing query analyzer settings.
NSS_CONSTANT(kConfigQueryAnalyzersNamespace, NamespaceString::kConfigDb, "queryAnalyzers"_sd)

// Namespace used for storing sampled queries.
NSS_CONSTANT(kConfigSampledQueriesNamespace, NamespaceString::kConfigDb, "sampledQueries"_sd)

// Namespace used for storing the diffs for sampled update queries.
NSS_CONSTANT(kConfigSampledQueriesDiffNamespace,
             NamespaceString::kConfigDb,
             "sampledQueriesDiff"_sd)

// Namespace used for the health log.
NSS_CONSTANT(kLocalHealthLogNamespace, NamespaceString::kLocalDb, "system.healthlog"_sd)

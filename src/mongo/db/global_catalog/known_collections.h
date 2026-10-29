// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

namespace mongo {
// Check for necessary restore procedures before adding collection to this set. If restore procedure
// is necessary, additions to this set should also be added to kCollectionEntries in
// 'configsvr_run_restore_command.cpp'.
const StringDataSet kConfigCollections{
    NamespaceString::kBlockFCVChangesNamespace.coll(),
    NamespaceString::kChangeStreamPreImagesNamespace.coll(),
    NamespaceString::kClusterParametersNamespace.coll(),
    NamespaceString::kCollectionCriticalSectionsNamespace.coll(),
    NamespaceString::kCompactStructuredEncryptionCoordinatorNamespace.coll(),
    NamespaceString::kConfigActionlogNamespace.coll(),
    NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace.coll(),
    NamespaceString::kConfigCacheDatabasesNamespace.coll(),
    NamespaceString::kConfigChangelogNamespace.coll(),
    NamespaceString::kConfigDatabasesNamespace.coll(),
    NamespaceString::kConfigImagesNamespace.coll(),
    NamespaceString::kConfigMongosNamespace.coll(),
    NamespaceString::kConfigQueryAnalyzersNamespace.coll(),
    NamespaceString::kConfigReshardingOperationsNamespace.coll(),
    NamespaceString::kConfigSampledQueriesDiffNamespace.coll(),
    NamespaceString::kConfigSampledQueriesNamespace.coll(),
    NamespaceString::kConfigSettingsNamespace.coll(),
    NamespaceString::kConfigShardCatalogChunksNamespace.coll(),
    NamespaceString::kConfigShardCatalogCollectionsNamespace.coll(),
    NamespaceString::kConfigShardCatalogDatabasesNamespace.coll(),
    NamespaceString::kConfigsvrChunksNamespace.coll(),
    NamespaceString::kConfigsvrCollectionsNamespace.coll(),
    NamespaceString::kConfigsvrCoordinatorsNamespace.coll(),
    NamespaceString::kConfigsvrPlacementHistoryNamespace.coll(),
    NamespaceString::kConfigsvrShardsNamespace.coll(),
    NamespaceString::kConfigsvrTagsNamespace.coll(),
    NamespaceString::kConfigVersionNamespace.coll(),
    NamespaceString::kDonorReshardingOperationsNamespace.coll(),
    NamespaceString::kExternalKeysCollectionNamespace.coll(),
    NamespaceString::kForceOplogBatchBoundaryNamespace.coll(),
    NamespaceString::kIndexBuildEntryNamespace.coll(),
    NamespaceString::kConfigMaxKeyOrphanScanStateNamespace.coll(),
    NamespaceString::kLogicalSessionsNamespace.coll(),
    NamespaceString::kMigrationCoordinatorsNamespace.coll(),
    NamespaceString::kMigrationRecipientsNamespace.coll(),
    NamespaceString::kConfigMaxKeyZoneScanStateNamespace.coll(),
    NamespaceString::kQueryShapeRepresentativeQueriesNamespace.coll(),
    NamespaceString::kRangeDeletionForRenameNamespace.coll(),
    NamespaceString::kRangeDeletionNamespace.coll(),
    NamespaceString::kRecipientReshardingOperationsNamespace.coll(),
    NamespaceString::kReshardingApplierProgressNamespace.coll(),
    NamespaceString::kReshardingTxnClonerProgressNamespace.coll(),
    NamespaceString::kSessionTransactionsTableNamespace.coll(),
    NamespaceString::kSetChangeStreamStateCoordinatorNamespace.coll(),
    NamespaceString::kShardConfigCollectionsNamespace.coll(),
    NamespaceString::kShardingDDLCoordinatorsNamespace.coll(),
    NamespaceString::kShardingRenameParticipantsNamespace.coll(),
    NamespaceString::kTransactionCoordinatorsNamespace.coll(),
    NamespaceString::kUserWritesCriticalSectionsNamespace.coll(),
    NamespaceString::kVectorClockNamespace.coll(),
};
}  // namespace mongo

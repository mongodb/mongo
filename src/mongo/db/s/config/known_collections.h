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
#include "mongo/db/namespace_string.h"
#include "mongo/util/string_map.h"

namespace mongo {
// Check for necessary restore procedures before adding collection to this set. If restore procedure
// is necessary, additions to this set should also be added to kCollectionEntries in
// 'configsvr_run_restore_command.cpp'.
const StringDataSet kConfigCollections{
    "actionlog",
    "changelog",
    "chunks",
    "migrations",
    "mongos",
    "movePrimaryRecipients",
    "system.preimages",
    "tags",
    "version",
    NamespaceString::kClusterParametersNamespace.coll(),
    NamespaceString::kCollectionCriticalSectionsNamespace.coll(),
    NamespaceString::kCompactStructuredEncryptionCoordinatorNamespace.coll(),
    NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace.coll(),
    NamespaceString::kConfigDatabasesNamespace.coll(),
    NamespaceString::kConfigImagesNamespace.coll(),
    NamespaceString::kConfigQueryAnalyzersNamespace.coll(),
    NamespaceString::kConfigReshardingOperationsNamespace.coll(),
    NamespaceString::kConfigSampledQueriesDiffNamespace.coll(),
    NamespaceString::kConfigSampledQueriesNamespace.coll(),
    NamespaceString::kConfigSettingsNamespace.coll(),
    NamespaceString::kConfigsvrCollectionsNamespace.coll(),
    NamespaceString::kConfigsvrCoordinatorsNamespace.coll(),
    NamespaceString::kConfigsvrIndexCatalogNamespace.coll(),
    NamespaceString::kConfigsvrPlacementHistoryNamespace.coll(),
    NamespaceString::kConfigsvrShardsNamespace.coll(),
    NamespaceString::kDistLocksNamepsace.coll(),
    NamespaceString::kDonorReshardingOperationsNamespace.coll(),
    NamespaceString::kExternalKeysCollectionNamespace.coll(),
    NamespaceString::kForceOplogBatchBoundaryNamespace.coll(),
    NamespaceString::kGlobalIndexClonerNamespace.coll(),
    NamespaceString::kIndexBuildEntryNamespace.coll(),
    NamespaceString::kLockpingsNamespace.coll(),
    NamespaceString::kLogicalSessionsNamespace.coll(),
    NamespaceString::kMigrationCoordinatorsNamespace.coll(),
    NamespaceString::kMigrationRecipientsNamespace.coll(),
    NamespaceString::kRangeDeletionForRenameNamespace.coll(),
    NamespaceString::kRangeDeletionNamespace.coll(),
    NamespaceString::kRecipientReshardingOperationsNamespace.coll(),
    NamespaceString::kReshardingApplierProgressNamespace.coll(),
    NamespaceString::kReshardingApplierProgressNamespace.coll(),
    NamespaceString::kReshardingTxnClonerProgressNamespace.coll(),
    NamespaceString::kSessionTransactionsTableNamespace.coll(),
    NamespaceString::kSessionTransactionsTableNamespace.coll(),
    NamespaceString::kSetChangeStreamStateCoordinatorNamespace.coll(),
    NamespaceString::kShardCollectionCatalogNamespace.coll(),
    NamespaceString::kShardConfigCollectionsNamespace.coll(),
    NamespaceString::kShardConfigDatabasesNamespace.coll(),
    NamespaceString::kShardIndexCatalogNamespace.coll(),
    NamespaceString::kShardingDDLCoordinatorsNamespace.coll(),
    NamespaceString::kShardingRenameParticipantsNamespace.coll(),
    NamespaceString::kShardSplitDonorsNamespace.coll(),
    NamespaceString::kTenantMigrationDonorsNamespace.coll(),
    NamespaceString::kTenantMigrationRecipientsNamespace.coll(),
    NamespaceString::kTransactionCoordinatorsNamespace.coll(),
    NamespaceString::kUserWritesCriticalSectionsNamespace.coll(),
    NamespaceString::kVectorClockNamespace.coll(),
};
}  // namespace mongo

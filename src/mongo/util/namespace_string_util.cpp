/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/util/namespace_string_util.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/str.h"
#include <ostream>

namespace mongo {

const stdx::unordered_set<NamespaceString> NamespaceStringUtil::globalNamespaces{
    NamespaceString::kServerConfigurationNamespace,
    NamespaceString::kLogicalSessionsNamespace,
    NamespaceString::kConfigDatabasesNamespace,
    NamespaceString::kSessionTransactionsTableNamespace,
    NamespaceString::kShardConfigCollectionsNamespace,
    NamespaceString::kShardConfigDatabasesNamespace,
    NamespaceString::kKeysCollectionNamespace,
    NamespaceString::kExternalKeysCollectionNamespace,
    NamespaceString::kRsOplogNamespace,
    NamespaceString::kTransactionCoordinatorsNamespace,
    NamespaceString::kMigrationCoordinatorsNamespace,
    NamespaceString::kMigrationRecipientsNamespace,
    NamespaceString::kTenantMigrationDonorsNamespace,
    NamespaceString::kTenantMigrationRecipientsNamespace,
    NamespaceString::kTenantMigrationOplogView,
    NamespaceString::kShardSplitDonorsNamespace,
    NamespaceString::kSystemReplSetNamespace,
    NamespaceString::kLastVoteNamespace,
    NamespaceString::kIndexBuildEntryNamespace,
    NamespaceString::kRangeDeletionNamespace,
    NamespaceString::kRangeDeletionForRenameNamespace,
    NamespaceString::kConfigReshardingOperationsNamespace,
    NamespaceString::kDonorReshardingOperationsNamespace,
    NamespaceString::kRecipientReshardingOperationsNamespace,
    NamespaceString::kShardingDDLCoordinatorsNamespace,
    NamespaceString::kShardingRenameParticipantsNamespace,
    NamespaceString::kConfigSettingsNamespace,
    NamespaceString::kVectorClockNamespace,
    NamespaceString::kReshardingApplierProgressNamespace,
    NamespaceString::kReshardingTxnClonerProgressNamespace,
    NamespaceString::kCollectionCriticalSectionsNamespace,
    NamespaceString::kForceOplogBatchBoundaryNamespace,
    NamespaceString::kConfigImagesNamespace,
    NamespaceString::kConfigsvrCoordinatorsNamespace,
    NamespaceString::kUserWritesCriticalSectionsNamespace,
    NamespaceString::kConfigsvrRestoreNamespace,
    NamespaceString::kCompactStructuredEncryptionCoordinatorNamespace,
    NamespaceString::kConfigsvrShardsNamespace,
    NamespaceString::kConfigsvrCollectionsNamespace,
    NamespaceString::kConfigsvrIndexCatalogNamespace,
    NamespaceString::kShardIndexCatalogNamespace,
    NamespaceString::kShardCollectionCatalogNamespace,
    NamespaceString::kConfigsvrPlacementHistoryNamespace,
    NamespaceString::kLockpingsNamespace,
    NamespaceString::kDistLocksNamepsace,
    NamespaceString::kSetChangeStreamStateCoordinatorNamespace,
    NamespaceString::kGlobalIndexClonerNamespace,
    NamespaceString::kConfigQueryAnalyzersNamespace,
    NamespaceString::kConfigSampledQueriesNamespace,
    NamespaceString::kConfigSampledQueriesDiffNamespace
};

std::string NamespaceStringUtil::serialize(const NamespaceString& ns) {
    if (gMultitenancySupport) {
        if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
            gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility)) {
            return ns.toString();
        }
        return ns.toStringWithTenantId();
    }
    return ns.toString();
}

NamespaceString NamespaceStringUtil::deserialize(boost::optional<TenantId> tenantId,
                                                 StringData ns) {
    if (ns.empty()) {
        return NamespaceString();
    }

    if (!gMultitenancySupport) {
        massert(6972102,
                str::stream() << "TenantId must not be set, but it is " << tenantId->toString(),
                tenantId == boost::none);
        return NamespaceString(boost::none, ns);
    }

    if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility)) {
        // TODO SERVER-62491: Invariant for all databases. Remove the invariant bypass for
        // admin, local, config dbs.
        auto nss = NamespaceString(std::move(tenantId), ns);
        if (!tenantId && !globalNamespaces.count(nss)) {
            massert(6972100, "fake assert", true);
           // std::cout<<"xxx nss isn't in global namespace and doesn't have tenant"<<std::endl;
        }
        return nss;
        /*StringData dbName = ns.substr(0, ns.find('.'));
        if (!(dbName == NamespaceString::kAdminDb) && !(dbName == NamespaceString::kLocalDb) &&
            !(dbName == NamespaceString::kConfigDb)) {
            massert(6972100,
                    str::stream() << "TenantId must be set on nss " << ns,
                    tenantId != boost::none);
        }
        return NamespaceString(std::move(tenantId), ns);*/
    }

    auto nss = NamespaceString::parseFromStringExpectTenantIdInMultitenancyMode(ns);
    // TenantId could be prefixed, or passed in separately (or both) and namespace is always
    // constructed with the tenantId separately.
    if (tenantId != boost::none) {
        if (!nss.tenantId()) {
            return NamespaceString(std::move(tenantId), ns);
        }
        massert(6972101,
                str::stream() << "TenantId must match the db prefix tenantId: "
                              << tenantId->toString() << " prefix " << nss.tenantId()->toString(),
                tenantId == nss.tenantId());
    }

    return nss;
}

NamespaceString NamespaceStringUtil::deserialize(DatabaseName dbName) {
    return NamespaceString(dbName);
}

NamespaceString NamespaceStringUtil::deserialize(DatabaseName dbName, StringData ns) {
    return NamespaceString(dbName, ns);
}

NamespaceString NamespaceStringUtil::deserialize(StringData ns, boost::optional<TenantId> tenantId) {
    return NamespaceStringUtil::deserialize(tenantId, ns);
}

NamespaceString NamespaceStringUtil::deserialize(StringData db, StringData coll, boost::optional<TenantId> tenantId) {
    return NamespaceStringUtil::deserialize(tenantId, str::stream() << db << "." << coll);
}

NamespaceString NamespaceStringUtil::deserialize(boost::optional<TenantId> tenantId, StringData db, StringData coll) {
    return NamespaceStringUtil::deserialize(tenantId, str::stream() << db << "." << coll);
}

}  // namespace mongo

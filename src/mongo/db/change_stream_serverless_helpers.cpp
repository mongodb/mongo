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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/db/change_stream_serverless_helpers.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"

namespace mongo {
namespace change_stream_serverless_helpers {

namespace {
bool isServerlessChangeStreamFeatureFlagEnabled() {
    return feature_flags::gFeatureFlagServerlessChangeStreams.isEnabled(
        serverGlobalParams.featureCompatibility);
}
}  // namespace

bool isChangeCollectionsModeActive() {
    // A change collection mode is declared as active if the required services can be initialized,
    // the feature flag is enabled and the FCV version is already initialized.
    return canInitializeServices() && isServerlessChangeStreamFeatureFlagEnabled();
}

bool isChangeStreamEnabled(OperationContext* opCtx, const TenantId& tenantId) {
    auto catalog = CollectionCatalog::get(opCtx);

    // A change stream in the serverless is declared as enabled if both the change collection and
    // the pre-images collection exist for the provided tenant.
    return isChangeCollectionsModeActive() &&
        static_cast<bool>(catalog->lookupCollectionByNamespace(
            opCtx, NamespaceString::makeChangeCollectionNSS(tenantId))) &&
        static_cast<bool>(catalog->lookupCollectionByNamespace(
            opCtx, NamespaceString::makePreImageCollectionNSS(tenantId)));
}

bool canInitializeServices() {
    // A change collection must not be enabled on the config server.
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        return false;
    }

    return canRunInTargetEnvironment();
}

bool canRunInTargetEnvironment() {
    // A change stream services are enabled only in the multitenant serverless settings. For the
    // sharded cluster, 'internalChangeStreamUseTenantIdForTesting' maybe provided for the testing
    // purposes until the support is available.
    const auto isMultiTenantServerless =
        getGlobalReplSettings().isServerless() && gMultitenancySupport;
    return isMultiTenantServerless || internalChangeStreamUseTenantIdForTesting.load();
}

const TenantId& getTenantIdForTesting() {
    static const TenantId kTestTenantId(
        OID("00000000"   /* timestamp */
            "0000000000" /* process id */
            "000000" /* counter */));

    return kTestTenantId;
}

boost::optional<TenantId> resolveTenantId(boost::optional<TenantId> tenantId) {
    if (isServerlessChangeStreamFeatureFlagEnabled()) {
        if (tenantId) {
            return tenantId;
        } else if (MONGO_unlikely(internalChangeStreamUseTenantIdForTesting.load())) {
            return getTenantIdForTesting();
        }
    }

    return boost::none;
}

TenantSet getConfigDbTenants(OperationContext* opCtx) {
    TenantSet tenantIds;

    auto dbNames = CollectionCatalog::get(opCtx)->getAllDbNames();
    for (auto&& dbName : dbNames) {
        if (dbName.db() == DatabaseName::kConfig.db() && dbName.tenantId()) {
            tenantIds.insert(*dbName.tenantId());
        }
    }

    return tenantIds;
}


int64_t getExpireAfterSeconds(const TenantId& tenantId) {
    auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
    auto* changeStreamsParam =
        clusterParameters->get<ClusterParameterWithStorage<ChangeStreamsClusterParameterStorage>>(
            "changeStreams");

    auto expireAfterSeconds = changeStreamsParam->getValue(tenantId).getExpireAfterSeconds();
    invariant(expireAfterSeconds > 0);
    return expireAfterSeconds;
}

}  // namespace change_stream_serverless_helpers
}  // namespace mongo

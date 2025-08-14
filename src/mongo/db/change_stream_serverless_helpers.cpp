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

#include "mongo/db/change_stream_serverless_helpers.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

MONGO_FAIL_POINT_DEFINE(injectCurrentWallTimeForChangeCollectionRemoval);

namespace change_stream_serverless_helpers {
namespace {
bool isServerlessChangeStreamFeatureFlagEnabled(const VersionContext& vCtx) {
    // We need to use isEnabledUseLastLTSFCVWhenUninitialized since this could run during startup
    // while the FCV is still uninitialized.
    return feature_flags::gFeatureFlagServerlessChangeStreams
        .isEnabledUseLastLTSFCVWhenUninitialized(
            vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}
}  // namespace

bool isChangeCollectionsModeActive(const VersionContext& vCtx) {
    // A change collection mode is declared as active if the required services can be initialized,
    // the feature flag is enabled and the FCV version is already initialized.
    return canInitializeServices() && isServerlessChangeStreamFeatureFlagEnabled(vCtx);
}

bool isChangeStreamEnabled(OperationContext* opCtx, const TenantId& tenantId) {
    auto catalog = CollectionCatalog::get(opCtx);

    // A change stream in the serverless is declared as enabled if the change collection exists for
    // the provided tenant. The pre-images collection is not supported on serverless.
    return isChangeCollectionsModeActive(VersionContext::getDecoration(opCtx)) &&
        static_cast<bool>(catalog->lookupCollectionByNamespace(
            opCtx, NamespaceString::makeChangeCollectionNSS(tenantId)));
}

bool canInitializeServices() {
    // A change collection must not be enabled on the config server.
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        return false;
    }

    return isServerlessEnvironment();
}

bool isServerlessEnvironment() {
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

boost::optional<TenantId> resolveTenantId(const VersionContext& vCtx,
                                          boost::optional<TenantId> tenantId) {
    if (isServerlessChangeStreamFeatureFlagEnabled(vCtx)) {
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
        if (dbName.isConfigDB() && dbName.tenantId()) {
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

Date_t getCurrentTimeForChangeCollectionRemoval(OperationContext* opCtx) {
    auto now = opCtx->fastClockSource().now();
    injectCurrentWallTimeForChangeCollectionRemoval.execute(
        [&](const BSONObj& data) { now = data.getField("currentWallTime").date(); });
    return now;
}
}  // namespace change_stream_serverless_helpers
}  // namespace mongo

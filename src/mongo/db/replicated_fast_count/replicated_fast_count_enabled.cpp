/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"

#include "mongo/db/feature_flag.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"

namespace mongo {
bool isReplicatedFastCountEnabled(OperationContext* opCtx) {
    // TODO(SERVER-117326): Remove feature flag check.
    return (rss::ReplicatedStorageService::get(opCtx)
                .getPersistenceProvider()
                .shouldUseReplicatedFastCount() ||
            gFeatureFlagReplicatedFastCount.isEnabledUseLatestFCVWhenUninitialized(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) &&
        repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();
}

bool isReplicatedFastCountEligible(const NamespaceString& nss) {
    // TODO(SERVER-120741): Allow if the local DB is the oplog.
    if (nss.isLocalDB() || nss.isImplicitlyReplicated() || nss.isServerConfigurationCollection() ||
        nss.isSystemDotProfile()) {
        return false;
    }
    // Exclude the fast count store collections themselves to avoid circular tracking.
    const auto fastCountStoreNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);
    const auto fastCountTimestampsNss = NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);
    return nss != fastCountStoreNss && nss != fastCountTimestampsNss;
}

}  // namespace mongo

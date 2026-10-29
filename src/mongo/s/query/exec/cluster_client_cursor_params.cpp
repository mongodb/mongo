// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/cluster_client_cursor_params.h"

#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"

namespace mongo {

void setRequestRemoteMetrics(const IncludeMetrics& remoteMetricsToInclude,
                             AsyncResultsMergerParams& armParams,
                             OperationContext* opCtx) {
    if (!hasAnyMetricsRequested(remoteMetricsToInclude)) {
        return;
    }
    // Use the new 'requestRemoteMetrics' field only when the feature flag is enabled, to avoid
    // sending an unrecognized field to shards that predate SERVER-127991. Fall back to the
    // deprecated 'requestQueryStatsFromRemotes' bool otherwise.
    // TODO (SERVER-126099): Remove the 'requestQueryStatsFromRemotes' field and always use
    // 'requestRemoteMetrics' once the feature flag is removed.
    if (feature_flags::gFeatureFlagIncludeMetricsObjectOption
            .isEnabledUseLastLTSFCVWhenUninitialized(
                opCtx ? VersionContext::getDecoration(opCtx) : VersionContext{},
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        armParams.setRequestRemoteMetrics(remoteMetricsToInclude);
    } else {
        armParams.setRequestQueryStatsFromRemotes(remoteMetricsToInclude.getQueryStats());
    }
}

ClusterClientCursorParams::ClusterClientCursorParams(
    NamespaceString nss,
    APIParameters apiParameters,
    boost::optional<ReadPreferenceSetting> readPreference,
    boost::optional<repl::ReadConcernArgs> readConcern,
    OperationSessionInfoFromClient osi)
    : nsString(std::move(nss)),
      apiParameters(std::move(apiParameters)),
      readPreference(std::move(readPreference)),
      readConcern(std::move(readConcern)),
      osi(std::move(osi)) {}

AsyncResultsMergerParams ClusterClientCursorParams::extractARMParams(OperationContext* opCtx) {
    AsyncResultsMergerParams armParams;
    if (!sortToApplyOnRouter.isEmpty()) {
        armParams.setSort(sortToApplyOnRouter);
    }
    armParams.setCompareWholeSortKey(compareWholeSortKeyOnRouter);
    armParams.setRemotes(std::move(remotes));
    armParams.setTailableMode(tailableMode);
    armParams.setBatchSize(batchSize);
    armParams.setNss(nsString);
    armParams.setAllowPartialResults(isAllowPartialResults);
    armParams.setOperationSessionInfo(osi);
    setRequestRemoteMetrics(remoteMetricsToInclude, armParams, opCtx);

    return armParams;
}

}  // namespace mongo

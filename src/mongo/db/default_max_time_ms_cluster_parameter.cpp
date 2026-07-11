// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/default_max_time_ms_cluster_parameter.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/default_max_time_ms_cluster_parameter_gen.h"

namespace mongo {
std::pair<boost::optional<Milliseconds>, bool> getRequestOrDefaultMaxTimeMS(
    OperationContext* opCtx,
    boost::optional<std::int64_t> requestMaxTimeMS,
    const bool isReadOperation) {

    // Always uses the user-defined value if it's passed in.
    if (requestMaxTimeMS) {
        return {Milliseconds{*requestMaxTimeMS}, false};
    }

    // Currently, defaultMaxTimeMS is only applicable to read operations.
    if (!isReadOperation) {
        return {boost::none, false};
    }

    const boost::optional<auth::ValidatedTenancyScope>& vts =
        auth::ValidatedTenancyScope::get(opCtx);
    auto tenantId = vts && vts->hasTenantId() ? boost::make_optional(vts->tenantId()) : boost::none;

    // Check if the defaultMaxTimeMS can be bypassed.
    const auto bypassDefaultMaxTimeMS =
        AuthorizationSession::get(opCtx->getClient())
            ->isAuthorizedForClusterAction(ActionType::bypassDefaultMaxTimeMS, tenantId);

    if (bypassDefaultMaxTimeMS) {
        return {boost::none, false};
    }

    auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
    auto* defaultMaxTimeMSParam =
        clusterParameters->get<ClusterParameterWithStorage<DefaultMaxTimeMSParam>>(
            "defaultMaxTimeMS");
    // Uses the tenant specific default value if one was set.
    if (tenantId) {
        auto tenantDefaultReadMaxTimeMS =
            defaultMaxTimeMSParam->getValue(tenantId).getReadOperations();
        if (tenantDefaultReadMaxTimeMS) {
            return {Milliseconds{tenantDefaultReadMaxTimeMS}, true};
        }
    }

    // Uses the global default maxTimeMS for read operations.
    auto globalDefaultReadMaxTimeMS =
        defaultMaxTimeMSParam->getValue(boost::none).getReadOperations();
    if (globalDefaultReadMaxTimeMS) {
        return {Milliseconds{globalDefaultReadMaxTimeMS}, true};
    }

    return {boost::none, false};
}
}  // namespace mongo

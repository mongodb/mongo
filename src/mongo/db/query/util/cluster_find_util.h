// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo::cluster_find_util {
/**
 * Helper function to detect when we are running find on a viewless timeseries query, converting
 * the request to an agg request, and calling runAggregate(). Returns CursorId if the conversion to
 * and execution as an aggregate pipeline took place.
 */
inline boost::optional<CursorId> convertFindAndRunAggregateIfViewlessTimeseries(
    OperationContext* const opCtx,
    RoutingContext& routingCtx,
    const NamespaceString& origNss,
    rpc::ReplyBuilderInterface* const result,
    const FindCommandRequest& request,
    const query_settings::QuerySettings& querySettings,
    boost::optional<mongo::ExplainOptions::Verbosity> verbosity = boost::none) {
    // If ns() is not present in the RoutingContext, this means that the nss it acquired was the
    // timeseries buckets namespace. We know for sure that this isn't a viewless timeseries, and can
    // bail out early.
    if (!routingCtx.hasNss(origNss)) {
        return boost::none;
    }
    const auto& cri = routingCtx.getCollectionRoutingInfo(origNss);
    if (timeseries::requiresViewlessTimeseriesTranslationInRouter(opCtx, cri)) {
        const auto hasExplain = verbosity.has_value();
        auto bodyBuilder = result->getBodyBuilder();
        bodyBuilder.resetToEmpty();
        auto aggRequest = query_request_conversion::asAggregateCommandRequest(request, hasExplain);
        aggRequest.setQuerySettings(querySettings);
        uassertStatusOK(ClusterAggregate::runAggregateWithRoutingCtx(
            opCtx,
            routingCtx,
            ClusterAggregate::Namespaces{origNss, origNss},
            aggRequest,
            {aggRequest} /* liteParsedPipeline */,
            {Privilege(ResourcePattern::forExactNamespace(origNss), ActionType::find)},
            boost::none /* resolvedView */,
            boost::none /* originalRequest */,
            verbosity,
            &bodyBuilder));

        // Pull the CursorId out of the cursor response and return it to the caller.
        return CursorId(
            bodyBuilder.asTempObj().getObjectField("cursor").getField("id").safeNumberLong());
    }
    return boost::none;
}
}  // namespace mongo::cluster_find_util

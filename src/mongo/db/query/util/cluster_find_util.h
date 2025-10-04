/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/util/assert_util.h"

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

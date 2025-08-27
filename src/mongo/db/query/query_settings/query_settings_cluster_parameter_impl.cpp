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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_settings/query_settings_usage_tracker.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/idl_parser.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>

namespace mongo::query_settings {

void QuerySettingsClusterParameter::append(OperationContext* opCtx,
                                           BSONObjBuilder* bob,
                                           StringData name,
                                           const boost::optional<TenantId>& tenantId) {
    auto& querySettingsService = QuerySettingsService::get(getGlobalServiceContext());
    auto config = querySettingsService.getAllQueryShapeConfigurations(tenantId);

    bob->append(QuerySettingsClusterParameterValue::k_idFieldName,
                querySettingsService.getQuerySettingsClusterParameterName());

    BSONArrayBuilder arrayBuilder(
        bob->subarrayStart(QuerySettingsClusterParameterValue::kSettingsArrayFieldName));
    for (auto&& item : config.queryShapeConfigurations) {
        arrayBuilder.append(item.toBSON());
    }
    arrayBuilder.done();
    bob->append(QuerySettingsClusterParameterValue::kClusterParameterTimeFieldName,
                config.clusterParameterTime.asTimestamp());
}

Status QuerySettingsClusterParameter::set(const BSONElement& newValueElement,
                                          const boost::optional<TenantId>& tenantId) {
    auto* serviceContext = getGlobalServiceContext();
    auto& querySettingsService = QuerySettingsService::get(serviceContext);
    auto newSettings = QuerySettingsClusterParameterValue::parse(
        newValueElement.Obj(),
        IDLParserContext("querySettingsParameterValue",
                         boost::none /* vts */,
                         tenantId,
                         SerializationContext::stateDefault()));

    // Skip installing the new settings if the incoming 'clusterParameterTime' did not change.
    // The cluster parameter time acts as the version of the configuration, meaning if it hasn't
    // changed, the configuration hasn't either, making this a no-op.
    if (newSettings.getClusterParameterTime() ==
        querySettingsService.getClusterParameterTime(tenantId)) {
        return Status::OK();
    }

    auto& settingsArray = newSettings.getSettingsArray();

    // TODO SERVER-97546 Remove PQS index hint sanitization.
    querySettingsService.sanitizeQuerySettingsHints(settingsArray);

    size_t rejectCount = 0;
    for (const auto& config : settingsArray) {
        if (config.getSettings().getReject()) {
            ++rejectCount;
        }
    }
    QuerySettingsUsageTracker::get(serviceContext)
        .setQuerySettingsUsageMetrics(/* count */ static_cast<int>(settingsArray.size()),
                                      /* size */ static_cast<int>(newValueElement.valuesize()),
                                      /* rejectCount */ static_cast<int>(rejectCount));
    querySettingsService.setAllQueryShapeConfigurations(
        {std::move(settingsArray), newSettings.getClusterParameterTime()}, tenantId);
    return Status::OK();
}

Status QuerySettingsClusterParameter::reset(const boost::optional<TenantId>& tenantId) {
    auto& querySettingsService = QuerySettingsService::get(getGlobalServiceContext());
    querySettingsService.removeAllQueryShapeConfigurations(tenantId);
    return Status::OK();
}

LogicalTime QuerySettingsClusterParameter::getClusterParameterTime(
    const boost::optional<TenantId>& tenantId) const {
    auto& querySettingsService = QuerySettingsService::get(getGlobalServiceContext());
    return querySettingsService.getClusterParameterTime(tenantId);
}
};  // namespace mongo::query_settings

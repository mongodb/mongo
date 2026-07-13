// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_knob_overrides.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_settings/query_settings_usage_tracker.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/idl_parser.h"

#include <string_view>

namespace mongo::query_settings {
namespace {

void logQueryKnobOverrideErrors(std::vector<QueryShapeConfiguration>& settingsArray) {
    for (auto& config : settingsArray) {
        auto& settings = config.getSettings();
        if (auto knobs = settings.getQueryKnobs()) {
            knobs->logErrors(BSON("queryShapeHash" << config.getQueryShapeHash().toHexString()));
            // These errors have now been reported; don't let them linger in the value that gets
            // cached in QuerySettingsManager, or a later per-query merge with a fresh, error-free
            // override (see lookupQuerySettingsWithRejectionCheck) would spuriously reject the
            // query over an already-handled, stale error.
            knobs->clearErrors();
            settings.setQueryKnobs(std::move(*knobs));
        }
    }
}

}  // namespace

void QuerySettingsClusterParameter::append(OperationContext* opCtx,
                                           BSONObjBuilder* bob,
                                           std::string_view name,
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

    // A knob override may have become invalid since it was accepted (e.g. removed, or its range
    // tightened) by the time this already-accepted value is (re-)applied via oplog application or
    // startup load. fromBSON() already dropped any such offending knob; just log it here rather
    // than letting it take down the node.
    logQueryKnobOverrideErrors(settingsArray);

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

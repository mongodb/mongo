// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_usage_tracker.h"

#include "mongo/db/commands/server_status/server_status.h"

namespace mongo::query_settings {
using namespace std::literals::string_view_literals;
namespace {

const auto getQuerySettingsUsageTracker =
    ServiceContext::declareDecoration<QuerySettingsUsageTracker>();

class QuerySettingsServerStatusSection final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        return QuerySettingsUsageTracker::get(opCtx).generateServerStatusSection(opCtx);
    }
};


auto& querySettingsServerStatusSection =
    *ServerStatusSectionBuilder<QuerySettingsServerStatusSection>("querySettings");
}  // namespace


QuerySettingsUsageTracker& QuerySettingsUsageTracker::get(ServiceContext* serviceContext) {
    return getQuerySettingsUsageTracker(serviceContext);
}

QuerySettingsUsageTracker& QuerySettingsUsageTracker::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

BSONObj QuerySettingsUsageTracker::generateServerStatusSection(OperationContext* opCtx) const {
    BSONObjBuilder root;
    serializeUsageMetrics(root);
    if (feature_flags::gFeatureFlagPQSBackfill.isEnabledUseLastLTSFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        BSONObjBuilder nested(root.subobjStart("backfill"));
        serializeBackfillMetrics(nested);
        nested.doneFast();
    }
    return root.obj();
}

void QuerySettingsUsageTracker::serializeUsageMetrics(BSONObjBuilder& bob) const {
    auto metrics = _usageMetrics.get();
    bob.append("count"sv, metrics.count);
    bob.append("size"sv, metrics.size);
    bob.append("rejectCount"sv, metrics.rejectCount);
}

void QuerySettingsUsageTracker::serializeBackfillMetrics(BSONObjBuilder& bob) const {
    bob.append("memoryUsedBytes"sv, _backfillMetrics.memoryUsedBytes.loadRelaxed());
    bob.append("missingRepresentativeQueries"sv,
               _backfillMetrics.missingRepresentativeQueries.loadRelaxed());
    bob.append("bufferedRepresentativeQueries"sv,
               _backfillMetrics.bufferedRepresentativeQueries.loadRelaxed());
    bob.append("insertedRepresentativeQueries"sv,
               _backfillMetrics.insertedRepresentativeQueries.loadRelaxed());
    bob.append("succeededBackfills"sv, _backfillMetrics.succeededBackfills.loadRelaxed());
    bob.append("failedBackfills"sv, _backfillMetrics.failedBackfills.loadRelaxed());
}

void QuerySettingsUsageTracker::setQuerySettingsUsageMetrics(int count, int size, int rejectCount) {
    *_usageMetrics = QuerySettingsUsageMetrics{
        .count = count,
        .size = size,
        .rejectCount = rejectCount,
    };
}

void QuerySettingsUsageTracker::setBackfillMemoryUsedBytes(int n) {
    _backfillMetrics.memoryUsedBytes.storeRelaxed(n);
}

void QuerySettingsUsageTracker::setMissingRepresentativeQueries(int n) {
    _backfillMetrics.missingRepresentativeQueries.storeRelaxed(n);
}

void QuerySettingsUsageTracker::setBufferedRepresentativeQueries(int n) {
    _backfillMetrics.bufferedRepresentativeQueries.storeRelaxed(n);
}

void QuerySettingsUsageTracker::incrementInsertedRepresentativeQueries(int n) {
    _backfillMetrics.insertedRepresentativeQueries.fetchAndAddRelaxed(n);
}

void QuerySettingsUsageTracker::incrementFailedBackfills(int n) {
    _backfillMetrics.failedBackfills.fetchAndAddRelaxed(n);
}

void QuerySettingsUsageTracker::incrementSucceededBackfills(int n) {
    _backfillMetrics.succeededBackfills.fetchAndAddRelaxed(n);
}
}  // namespace mongo::query_settings

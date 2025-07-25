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

#include "mongo/db/query/query_settings/query_settings_usage_tracker.h"

#include "mongo/db/commands/server_status.h"

namespace mongo::query_settings {
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
    bob.append("count"_sd, metrics.count);
    bob.append("size"_sd, metrics.size);
    bob.append("rejectCount"_sd, metrics.rejectCount);
}

void QuerySettingsUsageTracker::serializeBackfillMetrics(BSONObjBuilder& bob) const {
    bob.append("memoryUsedBytes"_sd, _backfillMetrics.memoryUsedBytes.loadRelaxed());
    bob.append("missingRepresentativeQueries"_sd,
               _backfillMetrics.missingRepresentativeQueries.loadRelaxed());
    bob.append("bufferedRepresentativeQueries"_sd,
               _backfillMetrics.bufferedRepresentativeQueries.loadRelaxed());
    bob.append("insertedRepresentativeQueries"_sd,
               _backfillMetrics.insertedRepresentativeQueries.loadRelaxed());
    bob.append("succeededBackfills"_sd, _backfillMetrics.succeededBackfills.loadRelaxed());
    bob.append("failedBackfills"_sd, _backfillMetrics.failedBackfills.loadRelaxed());
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

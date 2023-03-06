/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/metrics/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/metrics/with_oplog_application_count_metrics_also_updating_cumulative_metrics.h"
#include "mongo/db/s/metrics/with_oplog_application_latency_metrics_interface_updating_cumulative_metrics.h"
#include "mongo/db/s/metrics/with_phase_duration_management.h"
#include "mongo/db/s/metrics/with_state_management_for_instance_metrics.h"
#include "mongo/db/s/metrics/with_typed_cumulative_metrics_provider.h"
#include "mongo/db/s/move_primary/move_primary_cumulative_metrics.h"
#include "mongo/db/s/move_primary/move_primary_metrics_helpers.h"

namespace mongo {
namespace move_primary_metrics {

enum TimedPhase { kPlaceholder };
constexpr auto kNumTimedPhase = 1;

namespace detail {

using PartialBase1 = WithTypedCumulativeMetricsProvider<ShardingDataTransformInstanceMetrics,
                                                        MovePrimaryCumulativeMetrics>;

using PartialBase2 =
    WithStateManagementForInstanceMetrics<PartialBase1, MovePrimaryCumulativeMetrics::AnyState>;

using PartialBaseFinal = WithPhaseDurationManagement<PartialBase2, TimedPhase, kNumTimedPhase>;

using Base = WithOplogApplicationLatencyMetricsInterfaceUpdatingCumulativeMetrics<
    WithOplogApplicationCountMetricsAlsoUpdatingCumulativeMetrics<
        WithOplogApplicationCountMetrics<detail::PartialBaseFinal>>>;

}  // namespace detail
}  // namespace move_primary_metrics

class MovePrimaryMetrics : public move_primary_metrics::detail::Base {
public:
    using AnyState = MovePrimaryCumulativeMetrics::AnyState;
    using Base = move_primary_metrics::detail::Base;
    using TimedPhase = move_primary_metrics::TimedPhase;

    template <typename T>
    static auto initializeFrom(const T& document, ServiceContext* serviceContext) {
        return initializeFrom(
            document,
            serviceContext->getFastClockSource(),
            ShardingDataTransformCumulativeMetrics::getForMovePrimary(serviceContext));
    }

    template <typename T>
    static auto initializeFrom(const T& document,
                               ClockSource* clockSource,
                               ShardingDataTransformCumulativeMetrics* cumulativeMetrics) {
        static_assert(move_primary_metrics::isStateDocument<T>);
        return std::make_unique<MovePrimaryMetrics>(
            document.getMetadata(),
            move_primary_metrics::getRoleForStateDocument<T>(),
            clockSource,
            cumulativeMetrics,
            move_primary_metrics::getState(document));
    }

    MovePrimaryMetrics(const MovePrimaryCommonMetadata& metadata,
                       Role role,
                       ClockSource* clockSource,
                       ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
                       AnyState state);

    MovePrimaryMetrics(UUID instanceId,
                       BSONObj originalCommand,
                       NamespaceString nss,
                       Role role,
                       Date_t startTime,
                       ClockSource* clockSource,
                       ShardingDataTransformCumulativeMetrics* cumulativeMetrics);

    BSONObj reportForCurrentOp() const noexcept override;
    boost::optional<Milliseconds> getRecipientHighEstimateRemainingTimeMillis() const override;

private:
};

}  // namespace mongo

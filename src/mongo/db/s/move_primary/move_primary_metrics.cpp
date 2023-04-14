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

#include "mongo/db/s/move_primary/move_primary_metrics.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/move_primary/move_primary_metrics_field_name_provider.h"

namespace mongo {

namespace {
using TimedPhase = MovePrimaryMetrics::TimedPhase;
const auto kTimedPhaseNamesMap = [] {
    return MovePrimaryMetrics::TimedPhaseNameMap{
        {TimedPhase::kPlaceholder, "placeholderPhaseDurationSecs"}};
}();

BSONObj createOriginalCommand(const NamespaceString& database, const StringData& shard) {
    return Document{{"movePrimary", database.toString()}, {"to", shard}}.toBson();
}
}  // namespace

MovePrimaryMetrics::MovePrimaryMetrics(const MovePrimaryCommonMetadata& metadata,
                                       Role role,
                                       ClockSource* clockSource,
                                       ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
                                       AnyState state)
    : MovePrimaryMetrics{
          metadata.getMigrationId(),
          createOriginalCommand(metadata.getDatabaseName(), metadata.getToShardName()),
          metadata.getDatabaseName(),
          role,
          clockSource->now(),
          clockSource,
          cumulativeMetrics} {
    setState(state);
}

MovePrimaryMetrics::MovePrimaryMetrics(UUID instanceId,
                                       BSONObj originalCommand,
                                       NamespaceString nss,
                                       Role role,
                                       Date_t startTime,
                                       ClockSource* clockSource,
                                       ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : Base{std::move(instanceId),
           std::move(originalCommand),
           std::move(nss),
           role,
           startTime,
           clockSource,
           cumulativeMetrics,
           std::make_unique<MovePrimaryMetricsFieldNameProvider>()} {}

BSONObj MovePrimaryMetrics::reportForCurrentOp() const noexcept {
    BSONObjBuilder builder;
    reportDurationsForAllPhases<Seconds>(kTimedPhaseNamesMap, getClockSource(), &builder);
    builder.appendElementsUnique(Base::reportForCurrentOp());
    return builder.obj();
}

boost::optional<Milliseconds> MovePrimaryMetrics::getRecipientHighEstimateRemainingTimeMillis()
    const {
    return boost::none;
}

StringData MovePrimaryMetrics::getStateString() const noexcept {
    return stdx::visit(OverloadedVisitor{[](MovePrimaryRecipientStateEnum state) {
                                             return MovePrimaryRecipientState_serializer(state);
                                         },
                                         [](MovePrimaryDonorStateEnum state) {
                                             return MovePrimaryDonorState_serializer(state);
                                         }},
                       getState());
}

}  // namespace mongo

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/util/optional_util.h"

namespace mongo {
namespace {

using TimedPhase = ReshardingMetrics::TimedPhase;
const auto kTimedPhaseNamesMap = [] {
    return ReshardingMetrics::TimedPhaseNameMap{
        {TimedPhase::kCloning, "totalCopyTimeElapsedSecs"},
        {TimedPhase::kApplying, "totalApplyTimeElapsedSecs"},
        {TimedPhase::kCriticalSection, "totalCriticalSectionTimeElapsedSecs"}};
}();

inline ReshardingMetrics::State getDefaultState(ReshardingMetrics::Role role) {
    using Role = ReshardingMetrics::Role;
    switch (role) {
        case Role::kCoordinator:
            return CoordinatorStateEnum::kUnused;
        case Role::kRecipient:
            return RecipientStateEnum::kUnused;
        case Role::kDonor:
            return DonorStateEnum::kUnused;
    }
    MONGO_UNREACHABLE;
}

// Returns the originalCommand with the createIndexes, key and unique fields added.
BSONObj createOriginalCommand(const NamespaceString& nss, BSONObj shardKey) {

    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;

    return Doc{{"reshardCollection", V{StringData{nss.toString()}}},
               {"key", std::move(shardKey)},
               {"unique", V{StringData{"false"}}},
               {"collation", V{Doc{{"locale", V{StringData{"simple"}}}}}}}
        .toBson();
}

Date_t readStartTime(const CommonReshardingMetadata& metadata, ClockSource* fallbackSource) {
    if (const auto& startTime = metadata.getStartTime()) {
        return startTime.value();
    } else {
        return fallbackSource->now();
    }
}

}  // namespace

void ReshardingMetrics::ExternallyTrackedRecipientFields::accumulateFrom(
    const ReshardingOplogApplierProgress& progressDoc) {
    auto setOrAdd = [](auto& opt, auto add) {
        opt = opt.value_or(0) + add;
    };
    setOrAdd(insertsApplied, progressDoc.getInsertsApplied());
    setOrAdd(updatesApplied, progressDoc.getUpdatesApplied());
    setOrAdd(deletesApplied, progressDoc.getDeletesApplied());
    setOrAdd(writesToStashCollections, progressDoc.getWritesToStashCollections());
}

ReshardingMetrics::ReshardingMetrics(UUID instanceId,
                                     BSONObj shardKey,
                                     NamespaceString nss,
                                     Role role,
                                     Date_t startTime,
                                     ClockSource* clockSource,
                                     ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : ReshardingMetrics{std::move(instanceId),
                        shardKey,
                        std::move(nss),
                        role,
                        std::move(startTime),
                        clockSource,
                        cumulativeMetrics,
                        getDefaultState(role)} {}

ReshardingMetrics::ReshardingMetrics(UUID instanceId,
                                     BSONObj shardKey,
                                     NamespaceString nss,
                                     Role role,
                                     Date_t startTime,
                                     ClockSource* clockSource,
                                     ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
                                     State state)
    : Base{std::move(instanceId),
           createOriginalCommand(nss, std::move(shardKey)),
           nss,
           role,
           startTime,
           clockSource,
           cumulativeMetrics,
           std::make_unique<ReshardingMetricsFieldNameProvider>()},
      _ableToEstimateRemainingRecipientTime{!mustRestoreExternallyTrackedRecipientFields(state)},
      _scopedObserver(registerInstanceMetrics()),
      _reshardingFieldNames{static_cast<ReshardingMetricsFieldNameProvider*>(_fieldNames.get())} {
    setState(state);
}

ReshardingMetrics::ReshardingMetrics(const CommonReshardingMetadata& metadata,
                                     Role role,
                                     ClockSource* clockSource,
                                     ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
                                     State state)
    : ReshardingMetrics{metadata.getReshardingUUID(),
                        metadata.getReshardingKey().toBSON(),
                        metadata.getSourceNss(),
                        role,
                        readStartTime(metadata, clockSource),
                        clockSource,
                        cumulativeMetrics,
                        state} {}

ReshardingMetrics::ReshardingMetrics(const CommonReshardingMetadata& metadata,
                                     Role role,
                                     ClockSource* clockSource,
                                     ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : ReshardingMetrics{metadata.getReshardingUUID(),
                        metadata.getReshardingKey().toBSON(),
                        metadata.getSourceNss(),
                        role,
                        readStartTime(metadata, clockSource),
                        clockSource,
                        cumulativeMetrics,
                        getDefaultState(role)} {}

ReshardingMetrics::~ReshardingMetrics() {
    // Deregister the observer first to ensure that the observer will no longer be able to reach
    // this object while destructor is running.
    _scopedObserver.reset();
}

std::string ReshardingMetrics::createOperationDescription() const noexcept {
    return fmt::format("ReshardingMetrics{}Service {}",
                       ShardingDataTransformMetrics::getRoleName(_role),
                       _instanceId.toString());
}

ReshardingCumulativeMetrics* ReshardingMetrics::getReshardingCumulativeMetrics() {
    return dynamic_cast<ReshardingCumulativeMetrics*>(getCumulativeMetrics());
}

boost::optional<Milliseconds> ReshardingMetrics::getRecipientHighEstimateRemainingTimeMillis()
    const {
    if (!_ableToEstimateRemainingRecipientTime.load()) {
        return boost::none;
    }
    return resharding::estimateRemainingRecipientTime(
        getStartFor(TimedPhase::kApplying).has_value(),
        getBytesWrittenCount(),
        getApproxBytesToScanCount(),
        getElapsed<Seconds>(TimedPhase::kCloning, getClockSource()).value_or(Seconds{0}),
        getOplogEntriesApplied(),
        getOplogEntriesFetched(),
        getElapsed<Seconds>(TimedPhase::kApplying, getClockSource()).value_or(Seconds{0}));
}

std::unique_ptr<ReshardingMetrics> ReshardingMetrics::makeInstance(UUID instanceId,
                                                                   BSONObj shardKey,
                                                                   NamespaceString nss,
                                                                   Role role,
                                                                   Date_t startTime,
                                                                   ServiceContext* serviceContext) {
    auto cumulativeMetrics =
        ShardingDataTransformCumulativeMetrics::getForResharding(serviceContext);
    return std::make_unique<ReshardingMetrics>(instanceId,
                                               createOriginalCommand(nss, std::move(shardKey)),
                                               std::move(nss),
                                               role,
                                               startTime,
                                               serviceContext->getFastClockSource(),
                                               cumulativeMetrics,
                                               getDefaultState(role));
}

StringData ReshardingMetrics::getStateString() const noexcept {
    return stdx::visit(
        OverloadedVisitor{
            [](CoordinatorStateEnum state) { return CoordinatorState_serializer(state); },
            [](RecipientStateEnum state) { return RecipientState_serializer(state); },
            [](DonorStateEnum state) {
                return DonorState_serializer(state);
            }},
        getState());
}

BSONObj ReshardingMetrics::reportForCurrentOp() const noexcept {
    BSONObjBuilder builder;
    reportDurationsForAllPhases<Seconds>(
        kTimedPhaseNamesMap, getClockSource(), &builder, Seconds{0});
    if (_role == Role::kRecipient) {
        reportOplogApplicationCountMetrics(_reshardingFieldNames, &builder);
    }
    builder.appendElementsUnique(Base::reportForCurrentOp());
    return builder.obj();
}

void ReshardingMetrics::restoreRecipientSpecificFields(
    const ReshardingRecipientDocument& document) {
    auto metrics = document.getMetrics();
    if (!metrics) {
        return;
    }
    auto docsToCopy = metrics->getApproxDocumentsToCopy();
    auto bytesToCopy = metrics->getApproxBytesToCopy();
    if (docsToCopy && bytesToCopy) {
        setDocumentsToProcessCounts(*docsToCopy, *bytesToCopy);
    }
    auto docsCopied = metrics->getFinalDocumentsCopiedCount();
    auto bytesCopied = metrics->getFinalBytesCopiedCount();
    if (docsCopied && bytesCopied) {
        restoreDocumentsProcessed(*docsCopied, *bytesCopied);
    }
    restorePhaseDurationFields(document);
}

void ReshardingMetrics::restoreCoordinatorSpecificFields(
    const ReshardingCoordinatorDocument& document) {
    restorePhaseDurationFields(document);
}

void ReshardingMetrics::restoreExternallyTrackedRecipientFields(
    const ExternallyTrackedRecipientFields& values) {
    invokeIfAllSet(&ReshardingMetrics::restoreDocumentsProcessed,
                   values.documentCountCopied,
                   values.documentBytesCopied);
    invokeIfAllSet(&ReshardingMetrics::restoreOplogEntriesFetched, values.oplogEntriesFetched);
    invokeIfAllSet(&ReshardingMetrics::restoreOplogEntriesApplied, values.oplogEntriesApplied);
    invokeIfAllSet(&ReshardingMetrics::restoreUpdatesApplied, values.updatesApplied);
    invokeIfAllSet(&ReshardingMetrics::restoreInsertsApplied, values.insertsApplied);
    invokeIfAllSet(&ReshardingMetrics::restoreDeletesApplied, values.deletesApplied);
    invokeIfAllSet(&ReshardingMetrics::restoreWritesToStashCollections,
                   values.writesToStashCollections);
    _ableToEstimateRemainingRecipientTime.store(true);
}
}  // namespace mongo

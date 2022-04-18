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
#include "mongo/db/s/resharding/resharding_metrics_new.h"
#include "mongo/db/exec/document_value/document.h"


namespace mongo {
namespace {

inline ReshardingMetricsNew::State getDefaultState(ReshardingMetricsNew::Role role) {
    using Role = ReshardingMetricsNew::Role;
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
    try {
        const auto& startTime = metadata.getStartTime();
        tassert(6503901,
                "Metadata is missing start time despite feature flag being enabled",
                startTime.has_value());
        return startTime.get();
    } catch (const DBException&) {
        return fallbackSource->now();
    }
}

}  // namespace

ReshardingMetricsNew::ReshardingMetricsNew(
    UUID instanceId,
    BSONObj shardKey,
    NamespaceString nss,
    Role role,
    Date_t startTime,
    ClockSource* clockSource,
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : ShardingDataTransformInstanceMetrics{std::move(instanceId),
                                           createOriginalCommand(nss, std::move(shardKey)),
                                           nss,
                                           role,
                                           startTime,
                                           clockSource,
                                           cumulativeMetrics},
      _state{getDefaultState(role)} {}

ReshardingMetricsNew::ReshardingMetricsNew(
    const CommonReshardingMetadata& metadata,
    Role role,
    ClockSource* clockSource,
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : ReshardingMetricsNew{metadata.getReshardingUUID(),
                           metadata.getReshardingKey().toBSON(),
                           metadata.getSourceNss(),
                           role,
                           readStartTime(metadata, clockSource),
                           clockSource,
                           cumulativeMetrics} {}

std::string ReshardingMetricsNew::createOperationDescription() const noexcept {
    return fmt::format("ReshardingMetrics{}Service {}",
                       ShardingDataTransformMetrics::getRoleName(_role),
                       _instanceId.toString());
}

std::unique_ptr<ReshardingMetricsNew> ReshardingMetricsNew::makeInstance(
    UUID instanceId,
    BSONObj shardKey,
    NamespaceString nss,
    Role role,
    Date_t startTime,
    ServiceContext* serviceContext) {
    auto cumulativeMetrics =
        ShardingDataTransformCumulativeMetrics::getForResharding(serviceContext);
    return std::make_unique<ReshardingMetricsNew>(instanceId,
                                                  createOriginalCommand(nss, std::move(shardKey)),
                                                  std::move(nss),
                                                  role,
                                                  startTime,
                                                  serviceContext->getFastClockSource(),
                                                  cumulativeMetrics);
}

StringData ReshardingMetricsNew::getStateString() const noexcept {
    return stdx::visit(
        visit_helper::Overloaded{
            [](CoordinatorStateEnum state) { return CoordinatorState_serializer(state); },
            [](RecipientStateEnum state) { return RecipientState_serializer(state); },
            [](DonorStateEnum state) { return DonorState_serializer(state); }},
        _state.load());
}

void ReshardingMetricsNew::accumulateFrom(const ReshardingOplogApplierProgress& progressDoc) {
    invariant(_role == Role::kRecipient);

    accumulateValues(progressDoc.getInsertsApplied(),
                     progressDoc.getUpdatesApplied(),
                     progressDoc.getDeletesApplied(),
                     progressDoc.getWritesToStashCollections());
}

void ReshardingMetricsNew::restoreRecipientSpecificFields(
    const ReshardingRecipientDocument& document) {
    auto metrics = document.getMetrics();
    if (!metrics) {
        return;
    }
    auto docsToCopy = metrics->getApproxDocumentsToCopy();
    auto bytesToCopy = metrics->getApproxBytesToCopy();
    if (docsToCopy && bytesToCopy) {
        setDocumentsToCopyCounts(*docsToCopy, *bytesToCopy);
    }
    auto docsCopied = metrics->getFinalDocumentsCopiedCount();
    auto bytesCopied = metrics->getFinalBytesCopiedCount();
    if (docsCopied && bytesCopied) {
        restoreDocumentsCopied(*docsCopied, *bytesCopied);
    }
    restorePhaseDurationFields(document);
}

void ReshardingMetricsNew::restoreCoordinatorSpecificFields(
    const ReshardingCoordinatorDocument& document) {
    restorePhaseDurationFields(document);
}

}  // namespace mongo

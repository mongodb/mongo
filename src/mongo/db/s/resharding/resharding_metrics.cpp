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


namespace mongo {
namespace {

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
        return startTime.get();
    } else {
        return fallbackSource->now();
    }
}

}  // namespace

ReshardingMetrics::ReshardingMetrics(UUID instanceId,
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
                        cumulativeMetrics} {}

std::string ReshardingMetrics::createOperationDescription() const noexcept {
    return fmt::format("ReshardingMetrics{}Service {}",
                       ShardingDataTransformMetrics::getRoleName(_role),
                       _instanceId.toString());
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
                                               cumulativeMetrics);
}

StringData ReshardingMetrics::getStateString() const noexcept {
    return stdx::visit(
        visit_helper::Overloaded{
            [](CoordinatorStateEnum state) { return CoordinatorState_serializer(state); },
            [](RecipientStateEnum state) { return RecipientState_serializer(state); },
            [](DonorStateEnum state) { return DonorState_serializer(state); }},
        _state.load());
}

void ReshardingMetrics::accumulateFrom(const ReshardingOplogApplierProgress& progressDoc) {
    invariant(_role == Role::kRecipient);

    accumulateValues(progressDoc.getInsertsApplied(),
                     progressDoc.getUpdatesApplied(),
                     progressDoc.getDeletesApplied(),
                     progressDoc.getWritesToStashCollections());
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
        setDocumentsToCopyCounts(*docsToCopy, *bytesToCopy);
    }
    auto docsCopied = metrics->getFinalDocumentsCopiedCount();
    auto bytesCopied = metrics->getFinalBytesCopiedCount();
    if (docsCopied && bytesCopied) {
        restoreDocumentsCopied(*docsCopied, *bytesCopied);
    }
    restorePhaseDurationFields(document);
}

void ReshardingMetrics::restoreCoordinatorSpecificFields(
    const ReshardingCoordinatorDocument& document) {
    restorePhaseDurationFields(document);
}

ReshardingMetrics::DonorState::DonorState(DonorStateEnum enumVal) : _enumVal(enumVal) {}

ShardingDataTransformCumulativeMetrics::DonorStateEnum ReshardingMetrics::DonorState::toMetrics()
    const {
    using MetricsEnum = ShardingDataTransformCumulativeMetrics::DonorStateEnum;

    switch (_enumVal) {
        case DonorStateEnum::kUnused:
            return MetricsEnum::kUnused;

        case DonorStateEnum::kPreparingToDonate:
            return MetricsEnum::kPreparingToDonate;

        case DonorStateEnum::kDonatingInitialData:
            return MetricsEnum::kDonatingInitialData;

        case DonorStateEnum::kDonatingOplogEntries:
            return MetricsEnum::kDonatingOplogEntries;

        case DonorStateEnum::kPreparingToBlockWrites:
            return MetricsEnum::kPreparingToBlockWrites;

        case DonorStateEnum::kError:
            return MetricsEnum::kError;

        case DonorStateEnum::kBlockingWrites:
            return MetricsEnum::kBlockingWrites;

        case DonorStateEnum::kDone:
            return MetricsEnum::kDone;
        default:
            invariant(false,
                      str::stream() << "Unexpected resharding coordinator state: "
                                    << DonorState_serializer(_enumVal));
            MONGO_UNREACHABLE;
    }
}

DonorStateEnum ReshardingMetrics::DonorState::getState() const {
    return _enumVal;
}

ReshardingMetrics::RecipientState::RecipientState(RecipientStateEnum enumVal) : _enumVal(enumVal) {}

ShardingDataTransformCumulativeMetrics::RecipientStateEnum
ReshardingMetrics::RecipientState::toMetrics() const {
    using MetricsEnum = ShardingDataTransformCumulativeMetrics::RecipientStateEnum;

    switch (_enumVal) {
        case RecipientStateEnum::kUnused:
            return MetricsEnum::kUnused;

        case RecipientStateEnum::kAwaitingFetchTimestamp:
            return MetricsEnum::kAwaitingFetchTimestamp;

        case RecipientStateEnum::kCreatingCollection:
            return MetricsEnum::kCreatingCollection;

        case RecipientStateEnum::kCloning:
            return MetricsEnum::kCloning;

        case RecipientStateEnum::kApplying:
            return MetricsEnum::kApplying;

        case RecipientStateEnum::kError:
            return MetricsEnum::kError;

        case RecipientStateEnum::kStrictConsistency:
            return MetricsEnum::kStrictConsistency;

        case RecipientStateEnum::kDone:
            return MetricsEnum::kDone;

        default:
            invariant(false,
                      str::stream() << "Unexpected resharding coordinator state: "
                                    << RecipientState_serializer(_enumVal));
            MONGO_UNREACHABLE;
    }
}

RecipientStateEnum ReshardingMetrics::RecipientState::getState() const {
    return _enumVal;
}

ReshardingMetrics::CoordinatorState::CoordinatorState(CoordinatorStateEnum enumVal)
    : _enumVal(enumVal) {}

ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum
ReshardingMetrics::CoordinatorState::toMetrics() const {
    switch (_enumVal) {
        case CoordinatorStateEnum::kUnused:
            return ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kUnused;

        case CoordinatorStateEnum::kInitializing:
            return ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kInitializing;

        case CoordinatorStateEnum::kPreparingToDonate:
            return ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kPreparingToDonate;

        case CoordinatorStateEnum::kCloning:
            return ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kCloning;

        case CoordinatorStateEnum::kApplying:
            return ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kApplying;

        case CoordinatorStateEnum::kBlockingWrites:
            return ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kBlockingWrites;

        case CoordinatorStateEnum::kAborting:
            return ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kAborting;

        case CoordinatorStateEnum::kCommitting:
            return ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kCommitting;

        case CoordinatorStateEnum::kDone:
            return ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kDone;
        default:
            invariant(false,
                      str::stream() << "Unexpected resharding coordinator state: "
                                    << CoordinatorState_serializer(_enumVal));
            MONGO_UNREACHABLE;
    }
}

CoordinatorStateEnum ReshardingMetrics::CoordinatorState::getState() const {
    return _enumVal;
}

}  // namespace mongo

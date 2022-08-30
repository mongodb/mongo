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
    using optional_util::setOrAdd;
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
                        std::move(role),
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
    : ShardingDataTransformInstanceMetrics{std::move(instanceId),
                                           createOriginalCommand(nss, std::move(shardKey)),
                                           nss,
                                           role,
                                           startTime,
                                           clockSource,
                                           cumulativeMetrics,
                                           std::make_unique<ReshardingMetricsFieldNameProvider>()},
      _ableToEstimateRemainingRecipientTime{!mustRestoreExternallyTrackedRecipientFields(state)},
      _deletesApplied{0},
      _insertsApplied{0},
      _updatesApplied{0},
      _oplogEntriesApplied{0},
      _oplogEntriesFetched{0},
      _applyingStartTime{kNoDate},
      _applyingEndTime{kNoDate},
      _stateHolder{getReshardingCumulativeMetrics(), state},
      _scopedObserver(registerInstanceMetrics()),
      _reshardingFieldNames{static_cast<ReshardingMetricsFieldNameProvider*>(_fieldNames.get())} {}

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
    return resharding::estimateRemainingRecipientTime(_applyingStartTime.load() != kNoDate,
                                                      getBytesWrittenCount(),
                                                      getApproxBytesToScanCount(),
                                                      getCopyingElapsedTimeSecs(),
                                                      _oplogEntriesApplied.load(),
                                                      _oplogEntriesFetched.load(),
                                                      getApplyingElapsedTimeSecs());
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
            [](DonorStateEnum state) { return DonorState_serializer(state); }},
        _stateHolder.getState());
}

BSONObj ReshardingMetrics::reportForCurrentOp() const noexcept {
    BSONObjBuilder builder;
    switch (_role) {
        case Role::kCoordinator:
            builder.append(_reshardingFieldNames->getForApplyTimeElapsed(),
                           getApplyingElapsedTimeSecs().count());
            break;
        case Role::kDonor:
            break;
        case Role::kRecipient:
            builder.append(_reshardingFieldNames->getForApplyTimeElapsed(),
                           getApplyingElapsedTimeSecs().count());
            builder.append(_reshardingFieldNames->getForInsertsApplied(), _insertsApplied.load());
            builder.append(_reshardingFieldNames->getForUpdatesApplied(), _updatesApplied.load());
            builder.append(_reshardingFieldNames->getForDeletesApplied(), _deletesApplied.load());
            builder.append(_reshardingFieldNames->getForOplogEntriesApplied(),
                           _oplogEntriesApplied.load());
            builder.append(_reshardingFieldNames->getForOplogEntriesFetched(),
                           _oplogEntriesFetched.load());
            break;
        default:
            MONGO_UNREACHABLE;
    }
    builder.appendElementsUnique(ShardingDataTransformInstanceMetrics::reportForCurrentOp());
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

ReshardingMetrics::DonorState::DonorState(DonorStateEnum enumVal) : _enumVal(enumVal) {}

ReshardingCumulativeMetrics::DonorStateEnum ReshardingMetrics::DonorState::toMetrics() const {
    using MetricsEnum = ReshardingCumulativeMetrics::DonorStateEnum;

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

ReshardingCumulativeMetrics::RecipientStateEnum ReshardingMetrics::RecipientState::toMetrics()
    const {
    using MetricsEnum = ReshardingCumulativeMetrics::RecipientStateEnum;

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

ReshardingCumulativeMetrics::CoordinatorStateEnum ReshardingMetrics::CoordinatorState::toMetrics()
    const {
    using MetricsEnum = ReshardingCumulativeMetrics::CoordinatorStateEnum;

    switch (_enumVal) {
        case CoordinatorStateEnum::kUnused:
            return MetricsEnum::kUnused;

        case CoordinatorStateEnum::kInitializing:
            return MetricsEnum::kInitializing;

        case CoordinatorStateEnum::kPreparingToDonate:
            return MetricsEnum::kPreparingToDonate;

        case CoordinatorStateEnum::kCloning:
            return MetricsEnum::kCloning;

        case CoordinatorStateEnum::kApplying:
            return MetricsEnum::kApplying;

        case CoordinatorStateEnum::kBlockingWrites:
            return MetricsEnum::kBlockingWrites;

        case CoordinatorStateEnum::kAborting:
            return MetricsEnum::kAborting;

        case CoordinatorStateEnum::kCommitting:
            return MetricsEnum::kCommitting;

        case CoordinatorStateEnum::kDone:
            return MetricsEnum::kDone;
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

void ReshardingMetrics::onDeleteApplied() {
    _deletesApplied.addAndFetch(1);
    getReshardingCumulativeMetrics()->onDeleteApplied();
}

void ReshardingMetrics::onInsertApplied() {
    _insertsApplied.addAndFetch(1);
    getReshardingCumulativeMetrics()->onInsertApplied();
}

void ReshardingMetrics::onUpdateApplied() {
    _updatesApplied.addAndFetch(1);
    getReshardingCumulativeMetrics()->onUpdateApplied();
}

void ReshardingMetrics::onOplogEntriesFetched(int64_t numEntries, Milliseconds elapsed) {
    _oplogEntriesFetched.addAndFetch(numEntries);
    getReshardingCumulativeMetrics()->onOplogEntriesFetched(numEntries, elapsed);
}

void ReshardingMetrics::restoreOplogEntriesFetched(int64_t numEntries) {
    _oplogEntriesFetched.store(numEntries);
}

void ReshardingMetrics::onOplogEntriesApplied(int64_t numEntries) {
    _oplogEntriesApplied.addAndFetch(numEntries);
    getReshardingCumulativeMetrics()->onOplogEntriesApplied(numEntries);
}

void ReshardingMetrics::restoreOplogEntriesApplied(int64_t numEntries) {
    _oplogEntriesApplied.store(numEntries);
}

void ReshardingMetrics::restoreUpdatesApplied(int64_t count) {
    _updatesApplied.store(count);
}

void ReshardingMetrics::restoreInsertsApplied(int64_t count) {
    _insertsApplied.store(count);
}

void ReshardingMetrics::restoreDeletesApplied(int64_t count) {
    _deletesApplied.store(count);
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

void ReshardingMetrics::onLocalInsertDuringOplogFetching(Milliseconds elapsed) {
    getReshardingCumulativeMetrics()->onLocalInsertDuringOplogFetching(elapsed);
}

void ReshardingMetrics::onBatchRetrievedDuringOplogApplying(Milliseconds elapsed) {
    getReshardingCumulativeMetrics()->onBatchRetrievedDuringOplogApplying(elapsed);
}

void ReshardingMetrics::onOplogLocalBatchApplied(Milliseconds elapsed) {
    getReshardingCumulativeMetrics()->onOplogLocalBatchApplied(elapsed);
}

void ReshardingMetrics::onApplyingBegin() {
    _applyingStartTime.store(getClockSource()->now());
}

void ReshardingMetrics::onApplyingEnd() {
    _applyingEndTime.store(getClockSource()->now());
}

void ReshardingMetrics::restoreApplyingBegin(Date_t date) {
    _applyingStartTime.store(date);
}

void ReshardingMetrics::restoreApplyingEnd(Date_t date) {
    _applyingEndTime.store(date);
}

Date_t ReshardingMetrics::getApplyingBegin() const {
    return _applyingStartTime.load();
}

Date_t ReshardingMetrics::getApplyingEnd() const {
    return _applyingEndTime.load();
}

Seconds ReshardingMetrics::getApplyingElapsedTimeSecs() const {
    return getElapsed<Seconds>(_applyingStartTime, _applyingEndTime, getClockSource());
}
}  // namespace mongo

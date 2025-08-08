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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/s/resharding/resharding_metrics_common.h"
#include "mongo/db/s/resharding/resharding_metrics_field_names.h"
#include "mongo/db/s/resharding/resharding_metrics_observer_impl.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

constexpr auto kNoEstimate = Milliseconds{-1};

using TimedPhase = ReshardingMetrics::TimedPhase;
const auto kTimedPhaseNamesMap = [] {
    return ReshardingMetrics::TimedPhaseNameMap{
        {TimedPhase::kCloning, "totalCopyTimeElapsedSecs"},
        {TimedPhase::kApplying, "totalApplyTimeElapsedSecs"},
        {TimedPhase::kCriticalSection, "totalCriticalSectionTimeElapsedSecs"},
        {TimedPhase::kBuildingIndex, "totalIndexBuildTimeElapsedSecs"}};
}();

boost::optional<Milliseconds> readCoordinatorEstimate(const AtomicWord<Milliseconds>& field) {
    auto estimate = field.load();
    if (estimate == kNoEstimate) {
        return boost::none;
    }
    return estimate;
}

template <typename T>
void appendOptionalMillisecondsFieldAs(BSONObjBuilder& builder,
                                       StringData fieldName,
                                       const boost::optional<Milliseconds> value) {
    if (!value) {
        return;
    }
    builder.append(fieldName, durationCount<T>(*value));
}

// Returns the originalCommand with the createIndexes, key and unique fields added.
BSONObj createOriginalCommand(const NamespaceString& nss, BSONObj shardKey) {

    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;

    return Doc{
        {"reshardCollection",
         V{StringData{NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())}}},
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

ReshardingProvenanceEnum readProvenance(const CommonReshardingMetadata& metadata) {
    if (const auto& provenance = metadata.getProvenance()) {
        return provenance.get();
    }

    return ReshardingProvenanceEnum::kReshardCollection;
}

boost::optional<TimedPhase> getAssociatedTimedPhase(CoordinatorStateEnum coordinatorPhase) {
    switch (coordinatorPhase) {
        case CoordinatorStateEnum::kCloning:
            return TimedPhase::kCloning;
        case CoordinatorStateEnum::kApplying:
            return TimedPhase::kApplying;
        case CoordinatorStateEnum::kBlockingWrites:
            return TimedPhase::kCriticalSection;
        default:
            return boost::none;
    }
    MONGO_UNREACHABLE
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

ReshardingMetrics::ReshardingMetrics(const CommonReshardingMetadata& metadata,
                                     Role role,
                                     ClockSource* clockSource,
                                     ReshardingCumulativeMetrics* cumulativeMetrics)
    : ReshardingMetrics{metadata, role, clockSource, cumulativeMetrics, getDefaultState(role)} {}

ReshardingMetrics::ReshardingMetrics(const CommonReshardingMetadata& metadata,
                                     Role role,
                                     ClockSource* clockSource,
                                     ReshardingCumulativeMetrics* cumulativeMetrics,
                                     State state)
    : ReshardingMetrics{metadata.getReshardingUUID(),
                        metadata.getReshardingKey().toBSON(),
                        metadata.getSourceNss(),
                        role,
                        readStartTime(metadata, clockSource),
                        clockSource,
                        cumulativeMetrics,
                        state,
                        readProvenance(metadata)} {}

ReshardingMetrics::ReshardingMetrics(UUID instanceId,
                                     BSONObj shardKey,
                                     NamespaceString nss,
                                     Role role,
                                     Date_t startTime,
                                     ClockSource* clockSource,
                                     ReshardingCumulativeMetrics* cumulativeMetrics,
                                     State state,
                                     ReshardingProvenanceEnum provenance)
    : ReshardingMetrics{std::move(instanceId),
                        shardKey,
                        std::move(nss),
                        role,
                        std::move(startTime),
                        clockSource,
                        cumulativeMetrics,
                        state,
                        std::make_unique<ReshardingMetricsObserverImpl>(this),
                        provenance} {}

ReshardingMetrics::ReshardingMetrics(UUID instanceId,
                                     BSONObj shardKey,
                                     NamespaceString nss,
                                     Role role,
                                     Date_t startTime,
                                     ClockSource* clockSource,
                                     ReshardingCumulativeMetrics* cumulativeMetrics,
                                     State state,
                                     ObserverPtr observer,
                                     ReshardingProvenanceEnum provenance)
    : _instanceId{std::move(instanceId)},
      _originalCommand{createOriginalCommand(nss, std::move(shardKey))},
      _sourceNs{nss},
      _role{role},
      _startTime{startTime},
      _clockSource{clockSource},
      _observer{std::move(observer)},
      _cumulativeMetrics{cumulativeMetrics},
      _approxDocumentsToProcess{0},
      _documentsProcessed{0},
      _approxBytesToScan{0},
      _bytesWritten{0},
      _coordinatorHighEstimateRemainingTimeMillis{kNoEstimate},
      _coordinatorLowEstimateRemainingTimeMillis{kNoEstimate},
      _writesDuringCriticalSection{0},
      _ableToEstimateRemainingRecipientTime{!mustRestoreExternallyTrackedRecipientFields(state)},
      _isSameKeyResharding{false},
      _scopedObserver(registerInstanceMetrics()),
      _provenance{provenance} {
    setState(state);
}

ReshardingMetrics::~ReshardingMetrics() {
    // Deregister the observer first to ensure that the observer will no longer be able to reach
    // this object while destructor is running.
    _scopedObserver.reset();
}

ReshardingMetrics::State ReshardingMetrics::getDefaultState(Role role) {
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

std::string ReshardingMetrics::createOperationDescription() const {
    return fmt::format("ReshardingMetrics{}Service {}",
                       ReshardingMetricsCommon::getRoleName(_role),
                       _instanceId.toString());
}

ReshardingCumulativeMetrics* ReshardingMetrics::getReshardingCumulativeMetrics() {
    return dynamic_cast<ReshardingCumulativeMetrics*>(getCumulativeMetrics());
}

boost::optional<Milliseconds> ReshardingMetrics::getHighEstimateRemainingTimeMillis(
    CalculationLogOption logOption) const {
    switch (_role) {
        case Role::kRecipient:
            return getRecipientHighEstimateRemainingTimeMillis(logOption);
        case Role::kCoordinator:
            return readCoordinatorEstimate(_coordinatorHighEstimateRemainingTimeMillis);
        case Role::kDonor:
            break;
    }
    MONGO_UNREACHABLE;
}

boost::optional<Milliseconds> ReshardingMetrics::getLowEstimateRemainingTimeMillis() const {
    switch (_role) {
        case Role::kRecipient:
            return getHighEstimateRemainingTimeMillis();
        case Role::kCoordinator:
            return readCoordinatorEstimate(_coordinatorLowEstimateRemainingTimeMillis);
        case Role::kDonor:
            break;
    }
    MONGO_UNREACHABLE;
}

Date_t ReshardingMetrics::getStartTimestamp() const {
    return _startTime;
}

const UUID& ReshardingMetrics::getInstanceId() const {
    return _instanceId;
}

ReshardingMetrics::Role ReshardingMetrics::getRole() const {
    return _role;
}

void ReshardingMetrics::onDocumentsProcessed(int64_t documentCount,
                                             int64_t totalDocumentsSizeBytes,
                                             Milliseconds elapsed) {
    _documentsProcessed.addAndFetch(documentCount);
    _bytesWritten.addAndFetch(totalDocumentsSizeBytes);
    _cumulativeMetrics->onInsertsDuringCloning(documentCount, totalDocumentsSizeBytes, elapsed);
}

int64_t ReshardingMetrics::getDocumentsProcessedCount() const {
    return _documentsProcessed.load();
}

int64_t ReshardingMetrics::getBytesWrittenCount() const {
    return _bytesWritten.load();
}

int64_t ReshardingMetrics::getApproxBytesToScanCount() const {
    return _approxBytesToScan.load();
}

int64_t ReshardingMetrics::getWritesDuringCriticalSection() const {
    return _writesDuringCriticalSection.load();
}

void ReshardingMetrics::restoreDocumentsProcessed(int64_t documentCount,
                                                  int64_t totalDocumentsSizeBytes) {
    _documentsProcessed.store(documentCount);
    _bytesWritten.store(totalDocumentsSizeBytes);
}

void ReshardingMetrics::restoreWritesToStashCollections(int64_t writesToStashCollections) {
    _writesToStashCollections.store(writesToStashCollections);
}

void ReshardingMetrics::setDocumentsToProcessCounts(int64_t documentCount,
                                                    int64_t totalDocumentsSizeBytes) {
    _approxDocumentsToProcess.store(documentCount);
    _approxBytesToScan.store(totalDocumentsSizeBytes);
}

void ReshardingMetrics::setCoordinatorHighEstimateRemainingTimeMillis(Milliseconds milliseconds) {
    _coordinatorHighEstimateRemainingTimeMillis.store(milliseconds);
}

void ReshardingMetrics::setCoordinatorLowEstimateRemainingTimeMillis(Milliseconds milliseconds) {
    _coordinatorLowEstimateRemainingTimeMillis.store(milliseconds);
}

void ReshardingMetrics::onWriteDuringCriticalSection() {
    _writesDuringCriticalSection.addAndFetch(1);
    _cumulativeMetrics->onWriteDuringCriticalSection();
}

Seconds ReshardingMetrics::getOperationRunningTimeSecs() const {
    return duration_cast<Seconds>(_clockSource->now() - _startTime);
}

void ReshardingMetrics::onWriteToStashedCollections() {
    _writesToStashCollections.fetchAndAdd(1);
    _cumulativeMetrics->onWriteToStashedCollections();
}

void ReshardingMetrics::onReadDuringCriticalSection() {
    _readsDuringCriticalSection.fetchAndAdd(1);
    _cumulativeMetrics->onReadDuringCriticalSection();
}

void ReshardingMetrics::onCloningRemoteBatchRetrieval(Milliseconds elapsed) {
    _cumulativeMetrics->onCloningRemoteBatchRetrieval(elapsed);
}

ReshardingCumulativeMetrics* ReshardingMetrics::getCumulativeMetrics() {
    return _cumulativeMetrics;
}

ClockSource* ReshardingMetrics::getClockSource() const {
    return _clockSource;
}

void ReshardingMetrics::setLastOpEndingChunkImbalance(int64_t imbalanceCount) {
    _cumulativeMetrics->setLastOpEndingChunkImbalance(imbalanceCount);
}

void ReshardingMetrics::onInsertApplied() {
    _insertsApplied.fetchAndAdd(1);
    getCumulativeMetrics()->onInsertApplied();
}

void ReshardingMetrics::onUpdateApplied() {
    _updatesApplied.fetchAndAdd(1);
    getCumulativeMetrics()->onUpdateApplied();
}

void ReshardingMetrics::onDeleteApplied() {
    _deletesApplied.fetchAndAdd(1);
    getCumulativeMetrics()->onDeleteApplied();
}

void ReshardingMetrics::onOplogEntriesFetched(int64_t numEntries) {
    _oplogEntriesFetched.fetchAndAdd(numEntries);
    getCumulativeMetrics()->onOplogEntriesFetched(numEntries);
}

void ReshardingMetrics::onOplogEntriesApplied(int64_t numEntries) {
    _oplogEntriesApplied.fetchAndAdd(numEntries);
    getCumulativeMetrics()->onOplogEntriesApplied(numEntries);
}

void ReshardingMetrics::registerDonors(const std::vector<ShardId>& donorShardIds) {
    tassert(10626500,
            str::stream() << "Only recipients can register donors",
            _role == Role::kRecipient);

    std::unique_lock lock(_oplogLatencyMetricsMutex);

    tassert(10626501,
            str::stream() << "Cannot register donors multiple times",
            _oplogLatencyMetrics.empty());
    for (auto donorShardId : donorShardIds) {
        _oplogLatencyMetrics.emplace(donorShardId,
                                     std::make_unique<OplogLatencyMetrics>(donorShardId));
    }
}

void ReshardingMetrics::updateAverageTimeToFetchOplogEntries(const ShardId& donorShardId,
                                                             Milliseconds timeToFetch) {
    std::unique_lock lock(_oplogLatencyMetricsMutex);

    auto it = _oplogLatencyMetrics.find(donorShardId);
    uassert(10626502,
            str::stream() << "Cannot update the average time to fetch oplog entries for '"
                          << donorShardId << "' since it has not been registered",
            it != _oplogLatencyMetrics.end());
    it->second->updateAverageTimeToFetch(timeToFetch);
}

void ReshardingMetrics::updateAverageTimeToApplyOplogEntries(const ShardId& donorShardId,
                                                             Milliseconds timeToApply) {
    std::unique_lock lock(_oplogLatencyMetricsMutex);

    auto it = _oplogLatencyMetrics.find(donorShardId);
    uassert(10626504,
            str::stream() << "Cannot update the average time to apply oplog entries for '"
                          << donorShardId << "' since it has not been registered",
            it != _oplogLatencyMetrics.end());
    it->second->updateAverageTimeToApply(timeToApply);
}

boost::optional<Milliseconds> ReshardingMetrics::getAverageTimeToFetchOplogEntries(
    const ShardId& donorShardId) const {
    std::unique_lock lock(_oplogLatencyMetricsMutex);

    auto it = _oplogLatencyMetrics.find(donorShardId);
    uassert(10626503,
            str::stream() << "Cannot get the average time to fetch oplog entries for '"
                          << donorShardId << "' since it has not been registered",
            it != _oplogLatencyMetrics.end());
    return it->second->getAverageTimeToFetch();
}

boost::optional<Milliseconds> ReshardingMetrics::getAverageTimeToApplyOplogEntries(
    const ShardId& donorShardId) const {
    std::unique_lock lock(_oplogLatencyMetricsMutex);

    auto it = _oplogLatencyMetrics.find(donorShardId);
    uassert(10626505,
            str::stream() << "Cannot get the average time to apply oplog entries for '"
                          << donorShardId << "' since it has not been registered",
            it != _oplogLatencyMetrics.end());
    return it->second->getAverageTimeToApply();
}

boost::optional<Milliseconds> ReshardingMetrics::getMaxAverageTimeToFetchAndApplyOplogEntries(
    ReshardingMetrics::CalculationLogOption logOption) const {
    std::shared_lock lock(_oplogLatencyMetricsMutex);

    auto shouldLog = logOption == CalculationLogOption::Show;
    auto summaryBuilder = shouldLog ? boost::make_optional(BSONObjBuilder{}) : boost::none;
    auto appendOptionalTime =
        [](BSONObjBuilder* builder, StringData fieldName, boost::optional<Milliseconds> time) {
            if (time.has_value()) {
                builder->append(fieldName, time->count());
            } else {
                builder->appendNull(fieldName);
            }
        };

    boost::optional<Milliseconds> maxAvgTimeToFetchAndApply;
    bool incomplete = false;

    for (const auto& [donorShardId, metrics] : _oplogLatencyMetrics) {
        auto avgTimeToFetch = metrics->getAverageTimeToFetch();
        auto avgTimeToApply = metrics->getAverageTimeToApply();

        if (shouldLog) {
            BSONObjBuilder shardBuilder;
            appendOptionalTime(&shardBuilder, "timeToFetchMillis", avgTimeToFetch);
            appendOptionalTime(&shardBuilder, "timeToApplyMillis", avgTimeToApply);
            summaryBuilder->append(donorShardId, shardBuilder.obj());
        }

        if (!avgTimeToFetch.has_value() || !avgTimeToApply.has_value()) {
            maxAvgTimeToFetchAndApply = boost::none;
            incomplete = true;
            continue;
        }
        if (incomplete) {
            // At least one donor has incomplete metrics, so skip calculating the maximum average.
            continue;
        }

        auto avgTimeToFetchAndApply = *avgTimeToFetch + *avgTimeToApply;
        maxAvgTimeToFetchAndApply = maxAvgTimeToFetchAndApply
            ? std::max(*maxAvgTimeToFetchAndApply, avgTimeToFetchAndApply)
            : avgTimeToFetchAndApply;
    }

    if (shouldLog) {
        LOGV2(10605801,
              "Calculated the maximum average time to fetch and apply oplog entries",
              "maxAvgTimeToFetchAndApply"_attr = maxAvgTimeToFetchAndApply,
              "summary"_attr = summaryBuilder->obj());
    }

    return maxAvgTimeToFetchAndApply;
}

void ReshardingMetrics::onBatchRetrievedDuringOplogFetching(Milliseconds elapsed) {
    getCumulativeMetrics()->onBatchRetrievedDuringOplogFetching(elapsed);
}

void ReshardingMetrics::onLocalInsertDuringOplogFetching(const Milliseconds& elapsed) {
    getCumulativeMetrics()->onLocalInsertDuringOplogFetching(elapsed);
}

void ReshardingMetrics::onBatchRetrievedDuringOplogApplying(const Milliseconds& elapsed) {
    getCumulativeMetrics()->onBatchRetrievedDuringOplogApplying(elapsed);
}

void ReshardingMetrics::onOplogLocalBatchApplied(Milliseconds elapsed) {
    getCumulativeMetrics()->onOplogLocalBatchApplied(elapsed);
}

boost::optional<ReshardingMetricsTimeInterval> ReshardingMetrics::getIntervalFor(
    TimedPhase phase) const {
    return _phaseDurations.getIntervalFor(phase);
}

boost::optional<Date_t> ReshardingMetrics::getStartFor(TimedPhase phase) const {
    return _phaseDurations.getStartFor(phase);
}

boost::optional<Date_t> ReshardingMetrics::getEndFor(TimedPhase phase) const {
    return _phaseDurations.getEndFor(phase);
}

void ReshardingMetrics::setStartFor(TimedPhase phase, Date_t date) {
    _phaseDurations.setStartFor(phase, date);
}

void ReshardingMetrics::setEndFor(TimedPhase phase, Date_t date) {
    _phaseDurations.setEndFor(phase, date);
}

void ReshardingMetrics::setStartFor(CoordinatorStateEnum phase, Date_t date) {
    if (auto associatedPhase = getAssociatedTimedPhase(phase)) {
        setStartFor(*associatedPhase, date);
    }
}

void ReshardingMetrics::setEndFor(CoordinatorStateEnum phase, Date_t date) {
    if (auto associatedPhase = getAssociatedTimedPhase(phase)) {
        setEndFor(*associatedPhase, date);
    }
}

ReshardingMetrics::UniqueScopedObserver ReshardingMetrics::registerInstanceMetrics() {
    return _cumulativeMetrics->registerInstanceMetrics(_observer.get());
}

int64_t ReshardingMetrics::getInsertsApplied() const {
    return _insertsApplied.load();
}

int64_t ReshardingMetrics::getUpdatesApplied() const {
    return _updatesApplied.load();
}

int64_t ReshardingMetrics::getDeletesApplied() const {
    return _deletesApplied.load();
}

int64_t ReshardingMetrics::getOplogEntriesFetched() const {
    return _oplogEntriesFetched.load();
}

int64_t ReshardingMetrics::getOplogEntriesApplied() const {
    return _oplogEntriesApplied.load();
}

void ReshardingMetrics::restoreInsertsApplied(int64_t count) {
    _insertsApplied.store(count);
}

void ReshardingMetrics::restoreUpdatesApplied(int64_t count) {
    _updatesApplied.store(count);
}

void ReshardingMetrics::restoreDeletesApplied(int64_t count) {
    _deletesApplied.store(count);
}

void ReshardingMetrics::restoreOplogEntriesFetched(int64_t count) {
    _oplogEntriesFetched.store(count);
}

void ReshardingMetrics::restoreOplogEntriesApplied(int64_t count) {
    _oplogEntriesApplied.store(count);
}

boost::optional<Milliseconds> ReshardingMetrics::getRecipientHighEstimateRemainingTimeMillis(
    CalculationLogOption logOption) const {
    if (!_ableToEstimateRemainingRecipientTime.load()) {
        return boost::none;
    }

    if (resharding::gReshardingRemainingTimeEstimateBasedOnMovingAverage.load()) {
        // If the estimate based on moving average is available, return it. Otherwise, fall back to
        // the estimate not based on moving average.
        if (auto estimate = getMaxAverageTimeToFetchAndApplyOplogEntries(logOption)) {
            return *estimate;
        }
    }

    return resharding::estimateRemainingRecipientTime(
        getStartFor(TimedPhase::kApplying).has_value(),
        getBytesWrittenCount(),
        getApproxBytesToScanCount(),
        getElapsed<Milliseconds>(TimedPhase::kCloning, getClockSource()).value_or(Seconds{0}),
        getOplogEntriesApplied(),
        getOplogEntriesFetched(),
        getElapsed<Milliseconds>(TimedPhase::kApplying, getClockSource()).value_or(Seconds{0}));
}

std::unique_ptr<ReshardingMetrics> ReshardingMetrics::makeInstance_forTest(
    UUID instanceId,
    BSONObj shardKey,
    NamespaceString nss,
    Role role,
    Date_t startTime,
    ServiceContext* serviceContext) {
    auto cumulativeMetrics = ReshardingCumulativeMetrics::getForResharding(serviceContext);
    return std::make_unique<ReshardingMetrics>(instanceId,
                                               createOriginalCommand(nss, std::move(shardKey)),
                                               std::move(nss),
                                               role,
                                               startTime,
                                               serviceContext->getFastClockSource(),
                                               cumulativeMetrics,
                                               getDefaultState(role),
                                               ReshardingProvenanceEnum::kReshardCollection);
}

StringData ReshardingMetrics::getStateString() const {
    return visit(OverloadedVisitor{
                     [](CoordinatorStateEnum state) { return CoordinatorState_serializer(state); },
                     [](RecipientStateEnum state) { return RecipientState_serializer(state); },
                     [](DonorStateEnum state) {
                         return DonorState_serializer(state);
                     }},
                 getState());
}

BSONObj ReshardingMetrics::reportForCurrentOp() const {
    using namespace resharding_metrics::field_names;

    BSONObjBuilder builder;
    builder.append(kType, "op");
    builder.append(kDescription, createOperationDescription());
    builder.append(kOp, "command");
    builder.append(kNamespace,
                   NamespaceStringUtil::serialize(_sourceNs, SerializationContext::stateDefault()));
    builder.append(kOriginatingCommand, _originalCommand);
    builder.append(kOpTimeElapsed, getOperationRunningTimeSecs().count());
    builder.appendElements(BSON("provenance" << ReshardingProvenance_serializer(_provenance)));
    switch (_role) {
        case Role::kCoordinator:
            appendOptionalMillisecondsFieldAs<Seconds>(
                builder,
                kAllShardsHighestRemainingOperationTimeEstimatedSecs,
                getHighEstimateRemainingTimeMillis());
            appendOptionalMillisecondsFieldAs<Seconds>(
                builder,
                kAllShardsLowestRemainingOperationTimeEstimatedSecs,
                getLowEstimateRemainingTimeMillis());
            builder.append(kCoordinatorState, getStateString());
            builder.append(resharding_metrics::field_names::kIsSameKeyResharding,
                           _isSameKeyResharding.load());
            break;
        case Role::kDonor:
            builder.append(kDonorState, getStateString());
            builder.append(kCountWritesDuringCriticalSection, _writesDuringCriticalSection.load());
            builder.append(kCountReadsDuringCriticalSection, _readsDuringCriticalSection.load());
            break;
        case Role::kRecipient:
            builder.append(kRecipientState, getStateString());
            appendOptionalMillisecondsFieldAs<Seconds>(
                builder, kRemainingOpTimeEstimated, getHighEstimateRemainingTimeMillis());
            builder.append(kApproxDocumentsToCopy, _approxDocumentsToProcess.load());
            builder.append(kApproxBytesToCopy, _approxBytesToScan.load());
            builder.append(kBytesCopied, _bytesWritten.load());
            builder.append(kCountWritesToStashCollections, _writesToStashCollections.load());
            builder.append(kDocumentsCopied, _documentsProcessed.load());
            builder.append(kIndexesToBuild, _indexesToBuild.load());
            builder.append(kIndexesBuilt, _indexesBuilt.load());
            builder.append(kOplogEntriesFetched, getOplogEntriesFetched());
            builder.append(kOplogEntriesApplied, getOplogEntriesApplied());
            builder.append(kInsertsApplied, getInsertsApplied());
            builder.append(kUpdatesApplied, getUpdatesApplied());
            builder.append(kDeletesApplied, getDeletesApplied());
            break;
        default:
            MONGO_UNREACHABLE;
    }
    reportDurationsForAllPhases<Seconds>(
        kTimedPhaseNamesMap, getClockSource(), &builder, Seconds{0});
    return builder.obj();
}

void ReshardingMetrics::restoreRecipientSpecificFields(
    const ReshardingRecipientDocument& document) {
    const auto& donorShards = document.getDonorShards();
    std::vector<ShardId> donorShardIds(donorShards.size());
    std::transform(donorShards.begin(),
                   donorShards.end(),
                   donorShardIds.begin(),
                   [](auto donorShard) { return donorShard.getShardId(); });
    registerDonors(donorShardIds);

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
    restoreIndexBuildDurationFields(*metrics);
}

void ReshardingMetrics::restoreCoordinatorSpecificFields(
    const ReshardingCoordinatorDocument& document) {
    auto isSameKeyResharding =
        document.getForceRedistribution() && *document.getForceRedistribution();
    setIsSameKeyResharding(isSameKeyResharding);
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

void ReshardingMetrics::restoreIndexBuildDurationFields(const ReshardingRecipientMetrics& metrics) {
    auto indexBuildTime = metrics.getIndexBuildTime();
    if (indexBuildTime) {
        auto indexBuildBegin = indexBuildTime->getStart();
        if (indexBuildBegin) {
            setStartFor(TimedPhase::kBuildingIndex, *indexBuildBegin);
        }
        auto indexBuildEnd = indexBuildTime->getStop();
        if (indexBuildEnd) {
            setEndFor(TimedPhase::kBuildingIndex, *indexBuildEnd);
        }
    }
}

void ReshardingMetrics::reportPhaseDurations(BSONObjBuilder* builder) {
    invariant(builder);
    const auto fieldNames = ReshardingMetrics::TimedPhaseNameMap{
        {TimedPhase::kCloning, "copyDurationMs"},
        {TimedPhase::kBuildingIndex, "buildingIndexDurationMs"},
        {TimedPhase::kApplying, "applyDurationMs"},
        {TimedPhase::kCriticalSection, "criticalSectionDurationMs"},
    };
    reportDurationsForAllPhases<Milliseconds>(fieldNames, getClockSource(), builder);
}

void ReshardingMetrics::updateDonorCtx(DonorShardContext& donorCtx) {
    donorCtx.setWritesDuringCriticalSection(getWritesDuringCriticalSection());
    BSONObjBuilder bob;
    reportPhaseDurations(&bob);
    donorCtx.setPhaseDurations(bob.obj());
    donorCtx.setCriticalSectionInterval(getIntervalFor(TimedPhase::kCriticalSection));
}

void ReshardingMetrics::updateRecipientCtx(RecipientShardContext& recipientCtx) {
    if (!recipientCtx.getBytesCopied()) {
        recipientCtx.setBytesCopied(getBytesWrittenCount());
    }
    recipientCtx.setOplogFetched(getOplogEntriesFetched());
    recipientCtx.setOplogApplied(getOplogEntriesApplied());
    BSONObjBuilder bob;
    reportPhaseDurations(&bob);
    recipientCtx.setPhaseDurations(bob.obj());
}

void ReshardingMetrics::onStarted() {
    getReshardingCumulativeMetrics()->onStarted(_isSameKeyResharding.load(), _instanceId);
}

void ReshardingMetrics::onSuccess() {
    getReshardingCumulativeMetrics()->onSuccess(_isSameKeyResharding.load(), _instanceId);
}

void ReshardingMetrics::onFailure() {
    getReshardingCumulativeMetrics()->onFailure(_isSameKeyResharding.load(), _instanceId);
}

void ReshardingMetrics::onCanceled() {
    getReshardingCumulativeMetrics()->onCanceled(_isSameKeyResharding.load(), _instanceId);
}

void ReshardingMetrics::setIsSameKeyResharding(bool isSameKeyResharding) {
    _isSameKeyResharding.store(isSameKeyResharding);
}

void ReshardingMetrics::setIndexesToBuild(int64_t numIndexes) {
    _indexesToBuild.store(numIndexes);
}

void ReshardingMetrics::setIndexesBuilt(int64_t numIndexes) {
    _indexesBuilt.store(numIndexes);
}

ReshardingMetrics::OplogLatencyMetrics::OplogLatencyMetrics(ShardId donorShardId)
    : _donorShardId(donorShardId) {}

void ReshardingMetrics::OplogLatencyMetrics::updateAverageTimeToFetch(Milliseconds timeToFetch) {
    stdx::lock_guard<stdx::mutex> lk(_timeToFetchMutex);

    _avgTimeToFetch = [&]() -> Milliseconds {
        if (!_avgTimeToFetch) {
            return timeToFetch;
        }
        return Milliseconds{(long long)resharding::calculateExponentialMovingAverage(
            _avgTimeToFetch->count(),
            timeToFetch.count(),
            resharding::gReshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor
                .load())};
    }();
}

void ReshardingMetrics::OplogLatencyMetrics::updateAverageTimeToApply(Milliseconds timeToApply) {
    stdx::lock_guard<stdx::mutex> lk(_timeToApplyMutex);

    _avgTimeToApply = [&]() -> Milliseconds {
        if (!_avgTimeToApply) {
            return timeToApply;
        }
        return Milliseconds{(long long)resharding::calculateExponentialMovingAverage(
            _avgTimeToApply->count(),
            timeToApply.count(),
            resharding::gReshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor
                .load())};
    }();
}

boost::optional<Milliseconds> ReshardingMetrics::OplogLatencyMetrics::getAverageTimeToFetch()
    const {
    stdx::lock_guard<stdx::mutex> lk(_timeToFetchMutex);
    return _avgTimeToFetch;
}

boost::optional<Milliseconds> ReshardingMetrics::OplogLatencyMetrics::getAverageTimeToApply()
    const {
    stdx::lock_guard<stdx::mutex> lk(_timeToApplyMutex);
    return _avgTimeToApply;
}

}  // namespace mongo

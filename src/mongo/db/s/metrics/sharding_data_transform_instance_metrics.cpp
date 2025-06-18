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

#include "mongo/db/s/metrics/sharding_data_transform_instance_metrics.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/s/metrics/sharding_data_transform_metrics_observer.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {

namespace {
constexpr auto kNoEstimate = Milliseconds{-1};

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

}  // namespace

ShardingDataTransformInstanceMetrics::ShardingDataTransformInstanceMetrics(
    UUID instanceId,
    BSONObj originalCommand,
    NamespaceString sourceNs,
    Role role,
    Date_t startTime,
    ClockSource* clockSource,
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
    FieldNameProviderPtr fieldNames)
    : ShardingDataTransformInstanceMetrics{
          std::move(instanceId),
          std::move(originalCommand),
          std::move(sourceNs),
          role,
          startTime,
          clockSource,
          cumulativeMetrics,
          std::move(fieldNames),
          std::make_unique<ShardingDataTransformMetricsObserver>(this)} {}

ShardingDataTransformInstanceMetrics::ShardingDataTransformInstanceMetrics(
    UUID instanceId,
    BSONObj originalCommand,
    NamespaceString sourceNs,
    Role role,
    Date_t startTime,
    ClockSource* clockSource,
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
    FieldNameProviderPtr fieldNames,
    ObserverPtr observer)
    : _instanceId{std::move(instanceId)},
      _originalCommand{std::move(originalCommand)},
      _sourceNs{std::move(sourceNs)},
      _role{role},
      _fieldNames{std::move(fieldNames)},
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
      _writesDuringCriticalSection{0} {}

boost::optional<Milliseconds>
ShardingDataTransformInstanceMetrics::getHighEstimateRemainingTimeMillis(
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

boost::optional<Milliseconds>
ShardingDataTransformInstanceMetrics::getLowEstimateRemainingTimeMillis() const {
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

Date_t ShardingDataTransformInstanceMetrics::getStartTimestamp() const {
    return _startTime;
}

const UUID& ShardingDataTransformInstanceMetrics::getInstanceId() const {
    return _instanceId;
}

ShardingDataTransformInstanceMetrics::Role ShardingDataTransformInstanceMetrics::getRole() const {
    return _role;
}

std::string ShardingDataTransformInstanceMetrics::createOperationDescription() const {
    return fmt::format("ShardingDataTransformMetrics{}Service {}",
                       ShardingDataTransformMetrics::getRoleName(_role),
                       _instanceId.toString());
}

StringData ShardingDataTransformInstanceMetrics::getStateString() const {
    return "Unknown";
}

BSONObj ShardingDataTransformInstanceMetrics::reportForCurrentOp() const {

    BSONObjBuilder builder;
    builder.append(_fieldNames->getForType(), "op");
    builder.append(_fieldNames->getForDescription(), createOperationDescription());
    builder.append(_fieldNames->getForOp(), "command");
    builder.append(_fieldNames->getForNamespace(),
                   NamespaceStringUtil::serialize(_sourceNs, SerializationContext::stateDefault()));
    builder.append(_fieldNames->getForOriginatingCommand(), _originalCommand);
    builder.append(_fieldNames->getForOpTimeElapsed(), getOperationRunningTimeSecs().count());
    switch (_role) {
        case Role::kCoordinator:
            appendOptionalMillisecondsFieldAs<Seconds>(
                builder,
                _fieldNames->getForAllShardsHighestRemainingOperationTimeEstimatedSecs(),
                getHighEstimateRemainingTimeMillis());
            appendOptionalMillisecondsFieldAs<Seconds>(
                builder,
                _fieldNames->getForAllShardsLowestRemainingOperationTimeEstimatedSecs(),
                getLowEstimateRemainingTimeMillis());
            builder.append(_fieldNames->getForCoordinatorState(), getStateString());
            break;
        case Role::kDonor:
            builder.append(_fieldNames->getForDonorState(), getStateString());
            builder.append(_fieldNames->getForCountWritesDuringCriticalSection(),
                           _writesDuringCriticalSection.load());
            builder.append(_fieldNames->getForCountReadsDuringCriticalSection(),
                           _readsDuringCriticalSection.load());
            break;
        case Role::kRecipient:
            builder.append(_fieldNames->getForRecipientState(), getStateString());
            appendOptionalMillisecondsFieldAs<Seconds>(
                builder,
                _fieldNames->getForRemainingOpTimeEstimated(),
                getHighEstimateRemainingTimeMillis());
            builder.append(_fieldNames->getForApproxDocumentsToProcess(),
                           _approxDocumentsToProcess.load());
            builder.append(_fieldNames->getForApproxBytesToScan(), _approxBytesToScan.load());
            builder.append(_fieldNames->getForBytesWritten(), _bytesWritten.load());
            builder.append(_fieldNames->getForCountWritesToStashCollections(),
                           _writesToStashCollections.load());
            builder.append(_fieldNames->getForDocumentsProcessed(), _documentsProcessed.load());
            break;
        default:
            MONGO_UNREACHABLE;
    }

    return builder.obj();
}

void ShardingDataTransformInstanceMetrics::onDocumentsProcessed(int64_t documentCount,
                                                                int64_t totalDocumentsSizeBytes,
                                                                Milliseconds elapsed) {
    _documentsProcessed.addAndFetch(documentCount);
    _bytesWritten.addAndFetch(totalDocumentsSizeBytes);
    _cumulativeMetrics->onInsertsDuringCloning(documentCount, totalDocumentsSizeBytes, elapsed);
}

int64_t ShardingDataTransformInstanceMetrics::getDocumentsProcessedCount() const {
    return _documentsProcessed.load();
}

int64_t ShardingDataTransformInstanceMetrics::getBytesWrittenCount() const {
    return _bytesWritten.load();
}

int64_t ShardingDataTransformInstanceMetrics::getApproxBytesToScanCount() const {
    return _approxBytesToScan.load();
}

int64_t ShardingDataTransformInstanceMetrics::getWritesDuringCriticalSection() const {
    return _writesDuringCriticalSection.load();
}

void ShardingDataTransformInstanceMetrics::restoreDocumentsProcessed(
    int64_t documentCount, int64_t totalDocumentsSizeBytes) {
    _documentsProcessed.store(documentCount);
    _bytesWritten.store(totalDocumentsSizeBytes);
}

void ShardingDataTransformInstanceMetrics::restoreWritesToStashCollections(
    int64_t writesToStashCollections) {
    _writesToStashCollections.store(writesToStashCollections);
}

void ShardingDataTransformInstanceMetrics::setDocumentsToProcessCounts(
    int64_t documentCount, int64_t totalDocumentsSizeBytes) {
    _approxDocumentsToProcess.store(documentCount);
    _approxBytesToScan.store(totalDocumentsSizeBytes);
}

void ShardingDataTransformInstanceMetrics::setCoordinatorHighEstimateRemainingTimeMillis(
    Milliseconds milliseconds) {
    _coordinatorHighEstimateRemainingTimeMillis.store(milliseconds);
}

void ShardingDataTransformInstanceMetrics::setCoordinatorLowEstimateRemainingTimeMillis(
    Milliseconds milliseconds) {
    _coordinatorLowEstimateRemainingTimeMillis.store(milliseconds);
}

void ShardingDataTransformInstanceMetrics::onWriteDuringCriticalSection() {
    _writesDuringCriticalSection.addAndFetch(1);
    _cumulativeMetrics->onWriteDuringCriticalSection();
}

Seconds ShardingDataTransformInstanceMetrics::getOperationRunningTimeSecs() const {
    return duration_cast<Seconds>(_clockSource->now() - _startTime);
}

void ShardingDataTransformInstanceMetrics::onWriteToStashedCollections() {
    _writesToStashCollections.fetchAndAdd(1);
    _cumulativeMetrics->onWriteToStashedCollections();
}

void ShardingDataTransformInstanceMetrics::onReadDuringCriticalSection() {
    _readsDuringCriticalSection.fetchAndAdd(1);
    _cumulativeMetrics->onReadDuringCriticalSection();
}

void ShardingDataTransformInstanceMetrics::onCloningRemoteBatchRetrieval(Milliseconds elapsed) {
    _cumulativeMetrics->onCloningRemoteBatchRetrieval(elapsed);
}

ShardingDataTransformCumulativeMetrics*
ShardingDataTransformInstanceMetrics::getCumulativeMetrics() {
    return _cumulativeMetrics;
}

ReshardingCumulativeMetrics* ShardingDataTransformInstanceMetrics::getTypedCumulativeMetrics() {
    return dynamic_cast<ReshardingCumulativeMetrics*>(getCumulativeMetrics());
}

ClockSource* ShardingDataTransformInstanceMetrics::getClockSource() const {
    return _clockSource;
}

void ShardingDataTransformInstanceMetrics::onStarted() {
    _cumulativeMetrics->onStarted();
}

void ShardingDataTransformInstanceMetrics::onSuccess() {
    _cumulativeMetrics->onSuccess();
}

void ShardingDataTransformInstanceMetrics::onFailure() {
    _cumulativeMetrics->onFailure();
}

void ShardingDataTransformInstanceMetrics::onCanceled() {
    _cumulativeMetrics->onCanceled();
}

void ShardingDataTransformInstanceMetrics::setLastOpEndingChunkImbalance(int64_t imbalanceCount) {
    _cumulativeMetrics->setLastOpEndingChunkImbalance(imbalanceCount);
}

void ShardingDataTransformInstanceMetrics::onInsertApplied() {
    _insertsApplied.fetchAndAdd(1);
    getTypedCumulativeMetrics()->onInsertApplied();
}

void ShardingDataTransformInstanceMetrics::onUpdateApplied() {
    _updatesApplied.fetchAndAdd(1);
    getTypedCumulativeMetrics()->onUpdateApplied();
}

void ShardingDataTransformInstanceMetrics::onDeleteApplied() {
    _deletesApplied.fetchAndAdd(1);
    getTypedCumulativeMetrics()->onDeleteApplied();
}

void ShardingDataTransformInstanceMetrics::onOplogEntriesFetched(int64_t numEntries) {
    _oplogEntriesFetched.fetchAndAdd(numEntries);
    getTypedCumulativeMetrics()->onOplogEntriesFetched(numEntries);
}

void ShardingDataTransformInstanceMetrics::onOplogEntriesApplied(int64_t numEntries) {
    _oplogEntriesApplied.fetchAndAdd(numEntries);
    getTypedCumulativeMetrics()->onOplogEntriesApplied(numEntries);
}

void ShardingDataTransformInstanceMetrics::registerDonors(
    const std::vector<ShardId>& donorShardIds) {
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

void ShardingDataTransformInstanceMetrics::updateAverageTimeToFetchOplogEntries(
    const ShardId& donorShardId, Milliseconds timeToFetch) {
    std::shared_lock lock(_oplogLatencyMetricsMutex);

    auto it = _oplogLatencyMetrics.find(donorShardId);
    uassert(10626502,
            str::stream() << "Cannot update the average time to fetch oplog entries for '"
                          << donorShardId << "' since it has not been registered",
            it != _oplogLatencyMetrics.end());
    it->second->updateAverageTimeToFetch(timeToFetch);
}

void ShardingDataTransformInstanceMetrics::updateAverageTimeToApplyOplogEntries(
    const ShardId& donorShardId, Milliseconds timeToApply) {
    std::shared_lock lock(_oplogLatencyMetricsMutex);

    auto it = _oplogLatencyMetrics.find(donorShardId);
    uassert(10626504,
            str::stream() << "Cannot update the average time to apply oplog entries for '"
                          << donorShardId << "' since it has not been registered",
            it != _oplogLatencyMetrics.end());
    it->second->updateAverageTimeToApply(timeToApply);
}

boost::optional<Milliseconds>
ShardingDataTransformInstanceMetrics::getAverageTimeToFetchOplogEntries(
    const ShardId& donorShardId) const {
    std::shared_lock lock(_oplogLatencyMetricsMutex);

    auto it = _oplogLatencyMetrics.find(donorShardId);
    uassert(10626503,
            str::stream() << "Cannot get the average time to fetch oplog entries for '"
                          << donorShardId << "' since it has not been registered",
            it != _oplogLatencyMetrics.end());
    return it->second->getAverageTimeToFetch();
}

boost::optional<Milliseconds>
ShardingDataTransformInstanceMetrics::getAverageTimeToApplyOplogEntries(
    const ShardId& donorShardId) const {
    std::shared_lock lock(_oplogLatencyMetricsMutex);

    auto it = _oplogLatencyMetrics.find(donorShardId);
    uassert(10626505,
            str::stream() << "Cannot get the average time to apply oplog entries for '"
                          << donorShardId << "' since it has not been registered",
            it != _oplogLatencyMetrics.end());
    return it->second->getAverageTimeToApply();
}

void ShardingDataTransformInstanceMetrics::onBatchRetrievedDuringOplogFetching(
    Milliseconds elapsed) {
    getTypedCumulativeMetrics()->onBatchRetrievedDuringOplogFetching(elapsed);
}

void ShardingDataTransformInstanceMetrics::onLocalInsertDuringOplogFetching(
    const Milliseconds& elapsed) {
    getTypedCumulativeMetrics()->onLocalInsertDuringOplogFetching(elapsed);
}

void ShardingDataTransformInstanceMetrics::onBatchRetrievedDuringOplogApplying(
    const Milliseconds& elapsed) {
    getTypedCumulativeMetrics()->onBatchRetrievedDuringOplogApplying(elapsed);
}

void ShardingDataTransformInstanceMetrics::onOplogLocalBatchApplied(Milliseconds elapsed) {
    getTypedCumulativeMetrics()->onOplogLocalBatchApplied(elapsed);
}

boost::optional<ReshardingMetricsTimeInterval> ShardingDataTransformInstanceMetrics::getIntervalFor(
    PhaseEnum phase) const {
    return _phaseDurations.getIntervalFor(phase);
}

boost::optional<Date_t> ShardingDataTransformInstanceMetrics::getStartFor(PhaseEnum phase) const {
    return _phaseDurations.getStartFor(phase);
}

boost::optional<Date_t> ShardingDataTransformInstanceMetrics::getEndFor(PhaseEnum phase) const {
    return _phaseDurations.getEndFor(phase);
}

void ShardingDataTransformInstanceMetrics::setStartFor(PhaseEnum phase, Date_t date) {
    _phaseDurations.setStartFor(phase, date);
}

void ShardingDataTransformInstanceMetrics::setEndFor(PhaseEnum phase, Date_t date) {
    _phaseDurations.setEndFor(phase, date);
}

ShardingDataTransformInstanceMetrics::UniqueScopedObserver
ShardingDataTransformInstanceMetrics::registerInstanceMetrics() {
    return _cumulativeMetrics->registerInstanceMetrics(_observer.get());
}

int64_t ShardingDataTransformInstanceMetrics::getInsertsApplied() const {
    return _insertsApplied.load();
}

int64_t ShardingDataTransformInstanceMetrics::getUpdatesApplied() const {
    return _updatesApplied.load();
}

int64_t ShardingDataTransformInstanceMetrics::getDeletesApplied() const {
    return _deletesApplied.load();
}

int64_t ShardingDataTransformInstanceMetrics::getOplogEntriesFetched() const {
    return _oplogEntriesFetched.load();
}

int64_t ShardingDataTransformInstanceMetrics::getOplogEntriesApplied() const {
    return _oplogEntriesApplied.load();
}

boost::optional<Milliseconds>
ShardingDataTransformInstanceMetrics::getMaxAverageTimeToFetchAndApplyOplogEntries(
    CalculationLogOption logOption) const {
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

void ShardingDataTransformInstanceMetrics::restoreInsertsApplied(int64_t count) {
    _insertsApplied.store(count);
}

void ShardingDataTransformInstanceMetrics::restoreUpdatesApplied(int64_t count) {
    _updatesApplied.store(count);
}

void ShardingDataTransformInstanceMetrics::restoreDeletesApplied(int64_t count) {
    _deletesApplied.store(count);
}

void ShardingDataTransformInstanceMetrics::restoreOplogEntriesFetched(int64_t count) {
    _oplogEntriesFetched.store(count);
}

void ShardingDataTransformInstanceMetrics::restoreOplogEntriesApplied(int64_t count) {
    _oplogEntriesApplied.store(count);
}

ShardingDataTransformInstanceMetrics::OplogLatencyMetrics::OplogLatencyMetrics(ShardId donorShardId)
    : _donorShardId(donorShardId) {}

void ShardingDataTransformInstanceMetrics::OplogLatencyMetrics::updateAverageTimeToFetch(
    Milliseconds timeToFetch) {
    stdx::lock_guard<stdx::mutex> lk(_timeToFetchMutex);

    _avgTimeToFetch = [&]() -> Milliseconds {
        if (!_avgTimeToFetch) {
            return timeToFetch;
        }
        return Milliseconds{(int)resharding::calculateExponentialMovingAverage(
            _avgTimeToFetch->count(),
            timeToFetch.count(),
            resharding::gReshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor
                .load())};
    }();
}

void ShardingDataTransformInstanceMetrics::OplogLatencyMetrics::updateAverageTimeToApply(
    Milliseconds timeToApply) {
    stdx::lock_guard<stdx::mutex> lk(_timeToApplyMutex);

    _avgTimeToApply = [&]() -> Milliseconds {
        if (!_avgTimeToApply) {
            return timeToApply;
        }
        return Milliseconds{(int)resharding::calculateExponentialMovingAverage(
            _avgTimeToApply->count(),
            timeToApply.count(),
            resharding::gReshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor
                .load())};
    }();
}

boost::optional<Milliseconds>
ShardingDataTransformInstanceMetrics::OplogLatencyMetrics::getAverageTimeToFetch() const {
    stdx::lock_guard<stdx::mutex> lk(_timeToFetchMutex);
    return _avgTimeToFetch;
}

boost::optional<Milliseconds>
ShardingDataTransformInstanceMetrics::OplogLatencyMetrics::getAverageTimeToApply() const {
    stdx::lock_guard<stdx::mutex> lk(_timeToApplyMutex);
    return _avgTimeToApply;
}

}  // namespace mongo

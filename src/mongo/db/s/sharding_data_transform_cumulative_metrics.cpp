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

#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/global_index_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"

#include <cstdint>

#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
namespace mongo {

namespace {
constexpr auto kActive = "active";
constexpr auto kOldestActive = "oldestActive";
constexpr auto kLatencies = "latencies";
constexpr auto kCurrentInSteps = "currentInSteps";
constexpr auto kEstimateNotAvailable = -1;

struct Metrics {
    ReshardingCumulativeMetrics _resharding;
    GlobalIndexCumulativeMetrics _globalIndexes;
};
using MetricsPtr = std::unique_ptr<Metrics>;
const auto getMetrics = ServiceContext::declareDecoration<MetricsPtr>();

const auto metricsRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ShardingDataTransformMetrics",
    [](ServiceContext* ctx) { getMetrics(ctx) = std::make_unique<Metrics>(); }};
}  // namespace

ShardingDataTransformCumulativeMetrics* ShardingDataTransformCumulativeMetrics::getForResharding(
    ServiceContext* context) {
    auto& metrics = getMetrics(context);
    return &metrics->_resharding;
}

ShardingDataTransformCumulativeMetrics* ShardingDataTransformCumulativeMetrics::getForGlobalIndexes(
    ServiceContext* context) {
    auto& metrics = getMetrics(context);
    return &metrics->_globalIndexes;
}

ShardingDataTransformCumulativeMetrics::ShardingDataTransformCumulativeMetrics(
    const std::string& rootSectionName, std::unique_ptr<NameProvider> fieldNameProvider)
    : _rootSectionName{rootSectionName},
      _fieldNames{std::move(fieldNameProvider)},
      _instanceMetricsForAllRoles(ShardingDataTransformMetrics::kRoleCount),
      _operationWasAttempted{false} {}

ShardingDataTransformCumulativeMetrics::UniqueScopedObserver
ShardingDataTransformCumulativeMetrics::registerInstanceMetrics(const InstanceObserver* metrics) {
    _operationWasAttempted.store(true);
    auto role = metrics->getRole();
    auto it = insertMetrics(metrics, getMetricsSetForRole(role));
    return std::make_unique<ShardingDataTransformCumulativeMetrics::ScopedObserver>(
        this, role, std::move(it));
}

int64_t ShardingDataTransformCumulativeMetrics::getOldestOperationHighEstimateRemainingTimeMillis(
    Role role) const {
    return getOldestOperationEstimateRemainingTimeMillis(role, EstimateType::kHigh);
}

int64_t ShardingDataTransformCumulativeMetrics::getOldestOperationLowEstimateRemainingTimeMillis(
    Role role) const {
    return getOldestOperationEstimateRemainingTimeMillis(role, EstimateType::kLow);
}

int64_t ShardingDataTransformCumulativeMetrics::getOldestOperationEstimateRemainingTimeMillis(
    Role role, EstimateType type) const {

    stdx::unique_lock guard(_mutex);
    auto op = getOldestOperation(guard, role);
    if (!op) {
        return kEstimateNotAvailable;
    }
    auto estimate = getEstimate(op, type);
    return estimate ? estimate->count() : kEstimateNotAvailable;
}

boost::optional<Milliseconds> ShardingDataTransformCumulativeMetrics::getEstimate(
    const InstanceObserver* op, EstimateType type) const {
    switch (type) {
        case kHigh:
            return op->getHighEstimateRemainingTimeMillis();
        case kLow:
            return op->getLowEstimateRemainingTimeMillis();
    }
    MONGO_UNREACHABLE;
}

size_t ShardingDataTransformCumulativeMetrics::getObservedMetricsCount() const {
    stdx::unique_lock guard(_mutex);
    size_t count = 0;
    for (const auto& set : _instanceMetricsForAllRoles) {
        count += set.size();
    }
    return count;
}

size_t ShardingDataTransformCumulativeMetrics::getObservedMetricsCount(Role role) const {
    stdx::unique_lock guard(_mutex);
    return getMetricsSetForRole(role).size();
}

void ShardingDataTransformCumulativeMetrics::reportForServerStatus(BSONObjBuilder* bob) const {
    if (!_operationWasAttempted.load()) {
        return;
    }

    BSONObjBuilder root(bob->subobjStart(_rootSectionName));
    root.append(_fieldNames->getForCountStarted(), _countStarted.load());
    root.append(_fieldNames->getForCountSucceeded(), _countSucceeded.load());
    root.append(_fieldNames->getForCountFailed(), _countFailed.load());
    root.append(_fieldNames->getForCountCanceled(), _countCancelled.load());
    root.append(_fieldNames->getForLastOpEndingChunkImbalance(),
                _lastOpEndingChunkImbalance.load());
    {
        BSONObjBuilder active(bob->subobjStart(kActive));
        reportActive(&active);
    }
    {
        BSONObjBuilder oldest(bob->subobjStart(kOldestActive));
        reportOldestActive(&oldest);
    }
    {
        BSONObjBuilder latencies(bob->subobjStart(kLatencies));
        reportLatencies(&latencies);
    }
    {
        BSONObjBuilder steps(bob->subobjStart(kCurrentInSteps));
        reportCurrentInSteps(&steps);
    }
}

void ShardingDataTransformCumulativeMetrics::reportActive(BSONObjBuilder* bob) const {
    bob->append(_fieldNames->getForDocumentsProcessed(), _documentsProcessed.load());
    bob->append(_fieldNames->getForBytesWritten(), _bytesWritten.load());
    bob->append(_fieldNames->getForCountWritesToStashCollections(),
                _writesToStashedCollections.load());
    bob->append(_fieldNames->getForCountWritesDuringCriticalSection(),
                _writesDuringCriticalSection.load());
    bob->append(_fieldNames->getForCountReadsDuringCriticalSection(),
                _readsDuringCriticalSection.load());
}

void ShardingDataTransformCumulativeMetrics::reportOldestActive(BSONObjBuilder* bob) const {
    bob->append(
        _fieldNames->getForCoordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis(),
        getOldestOperationHighEstimateRemainingTimeMillis(Role::kCoordinator));
    bob->append(
        _fieldNames->getForCoordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis(),
        getOldestOperationLowEstimateRemainingTimeMillis(Role::kCoordinator));
    bob->append(_fieldNames->getForRecipientRemainingOperationTimeEstimatedMillis(),
                getOldestOperationHighEstimateRemainingTimeMillis(Role::kRecipient));
}

void ShardingDataTransformCumulativeMetrics::reportLatencies(BSONObjBuilder* bob) const {
    bob->append(_fieldNames->getForCollectionCloningTotalRemoteBatchRetrievalTimeMillis(),
                _totalBatchRetrievedDuringCloneMillis.load());
    bob->append(_fieldNames->getForCollectionCloningTotalRemoteBatchesRetrieved(),
                _totalBatchRetrievedDuringClone.load());
    bob->append(_fieldNames->getForCollectionCloningTotalLocalInsertTimeMillis(),
                _collectionCloningTotalLocalInsertTimeMillis.load());
    bob->append(_fieldNames->getForCollectionCloningTotalLocalInserts(),
                _collectionCloningTotalLocalBatchInserts.load());
}

void ShardingDataTransformCumulativeMetrics::reportCurrentInSteps(BSONObjBuilder* bob) const {
    // Do nothing.
}

const ShardingDataTransformCumulativeMetrics::InstanceObserver*
ShardingDataTransformCumulativeMetrics::getOldestOperation(WithLock, Role role) const {
    auto set = getMetricsSetForRole(role);
    if (set.empty()) {
        return nullptr;
    }
    return *set.begin();
}

ShardingDataTransformCumulativeMetrics::MetricsSet&
ShardingDataTransformCumulativeMetrics::getMetricsSetForRole(Role role) {
    return _instanceMetricsForAllRoles[static_cast<size_t>(role)];
}

const ShardingDataTransformCumulativeMetrics::MetricsSet&
ShardingDataTransformCumulativeMetrics::getMetricsSetForRole(Role role) const {
    return _instanceMetricsForAllRoles[static_cast<size_t>(role)];
}

ShardingDataTransformCumulativeMetrics::MetricsSet::iterator
ShardingDataTransformCumulativeMetrics::insertMetrics(const InstanceObserver* metrics,
                                                      MetricsSet& set) {
    stdx::unique_lock guard(_mutex);
    auto before = set.size();
    auto it = set.insert(set.end(), metrics);
    invariant(before + 1 == set.size());
    return it;
}

void ShardingDataTransformCumulativeMetrics::deregisterMetrics(
    const Role& role,
    const ShardingDataTransformCumulativeMetrics::MetricsSet::iterator& metricsIterator) {
    stdx::unique_lock guard(_mutex);
    getMetricsSetForRole(role).erase(metricsIterator);
}

void ShardingDataTransformCumulativeMetrics::onStarted() {
    _countStarted.fetchAndAdd(1);
}

void ShardingDataTransformCumulativeMetrics::onSuccess() {
    _countSucceeded.fetchAndAdd(1);
}

void ShardingDataTransformCumulativeMetrics::onFailure() {
    _countFailed.fetchAndAdd(1);
}

void ShardingDataTransformCumulativeMetrics::onCanceled() {
    _countCancelled.fetchAndAdd(1);
}

void ShardingDataTransformCumulativeMetrics::setLastOpEndingChunkImbalance(int64_t imbalanceCount) {
    _lastOpEndingChunkImbalance.store(imbalanceCount);
}

void ShardingDataTransformCumulativeMetrics::onInsertsDuringCloning(
    int64_t count, int64_t bytes, const Milliseconds& elapsedTime) {
    _collectionCloningTotalLocalBatchInserts.fetchAndAdd(1);
    _documentsProcessed.fetchAndAdd(count);
    _bytesWritten.fetchAndAdd(bytes);
    _collectionCloningTotalLocalInsertTimeMillis.fetchAndAdd(
        durationCount<Milliseconds>(elapsedTime));
}

void ShardingDataTransformCumulativeMetrics::onReadDuringCriticalSection() {
    _readsDuringCriticalSection.fetchAndAdd(1);
}

void ShardingDataTransformCumulativeMetrics::onWriteDuringCriticalSection() {
    _writesDuringCriticalSection.fetchAndAdd(1);
}

void ShardingDataTransformCumulativeMetrics::onWriteToStashedCollections() {
    _writesToStashedCollections.fetchAndAdd(1);
}

void ShardingDataTransformCumulativeMetrics::onCloningTotalRemoteBatchRetrieval(
    Milliseconds elapsed) {
    _totalBatchRetrievedDuringClone.fetchAndAdd(1);
    _totalBatchRetrievedDuringCloneMillis.fetchAndAdd(durationCount<Milliseconds>(elapsed));
}

const ShardingDataTransformCumulativeMetricsFieldNameProvider*
ShardingDataTransformCumulativeMetrics::getFieldNames() const {
    return _fieldNames.get();
}

ShardingDataTransformCumulativeMetrics::ScopedObserver::ScopedObserver(
    ShardingDataTransformCumulativeMetrics* metrics,
    Role role,
    MetricsSet::iterator observerIterator)
    : _metrics(metrics), _role(role), _observerIterator(std::move(observerIterator)) {}

ShardingDataTransformCumulativeMetrics::ScopedObserver::~ScopedObserver() {
    _metrics->deregisterMetrics(_role, _observerIterator);
}

}  // namespace mongo

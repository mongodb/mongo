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

#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"

#include "mongo/db/s/resharding/resharding_metrics_field_names.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"

#include <array>
#include <memory>
#include <utility>
#include <variant>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

constexpr auto kEstimateNotAvailable = -1;

constexpr auto kActive = "active";
constexpr auto kOldestActive = "oldestActive";
constexpr auto kLatencies = "latencies";
constexpr auto kCurrentInSteps = "currentInSteps";

constexpr auto kResharding = "resharding";

const auto kReportedStateFieldNamesMap = [] {
    return ReshardingCumulativeMetrics::StateTracker::StateFieldNameMap{
        {CoordinatorStateEnum::kInitializing, "countInstancesInCoordinatorState1Initializing"},
        {CoordinatorStateEnum::kPreparingToDonate,
         "countInstancesInCoordinatorState2PreparingToDonate"},
        {CoordinatorStateEnum::kCloning, "countInstancesInCoordinatorState3Cloning"},
        {CoordinatorStateEnum::kApplying, "countInstancesInCoordinatorState4Applying"},
        {CoordinatorStateEnum::kBlockingWrites, "countInstancesInCoordinatorState5BlockingWrites"},
        {CoordinatorStateEnum::kAborting, "countInstancesInCoordinatorState6Aborting"},
        {CoordinatorStateEnum::kCommitting, "countInstancesInCoordinatorState7Committing"},
        {DonorStateEnum::kPreparingToDonate, "countInstancesInDonorState1PreparingToDonate"},
        {DonorStateEnum::kDonatingInitialData, "countInstancesInDonorState2DonatingInitialData"},
        {DonorStateEnum::kDonatingOplogEntries, "countInstancesInDonorState3DonatingOplogEntries"},
        {DonorStateEnum::kPreparingToBlockWrites,
         "countInstancesInDonorState4PreparingToBlockWrites"},
        {DonorStateEnum::kError, "countInstancesInDonorState5Error"},
        {DonorStateEnum::kBlockingWrites, "countInstancesInDonorState6BlockingWrites"},
        {DonorStateEnum::kDone, "countInstancesInDonorState7Done"},
        {RecipientStateEnum::kAwaitingFetchTimestamp,
         "kCountInstancesInRecipientState1AwaitingFetchTimestamp"},
        {RecipientStateEnum::kCreatingCollection,
         "countInstancesInRecipientState2CreatingCollection"},
        {RecipientStateEnum::kCloning, "countInstancesInRecipientState3Cloning"},
        {RecipientStateEnum::kBuildingIndex, "countInstancesInRecipientState4BuildingIndex"},
        {RecipientStateEnum::kApplying, "countInstancesInRecipientState5Applying"},
        {RecipientStateEnum::kError, "countInstancesInRecipientState6Error"},
        {RecipientStateEnum::kStrictConsistency,
         "countInstancesInRecipientState7StrictConsistency"},
        {RecipientStateEnum::kDone, "countInstancesInRecipientState8Done"},
    };
}();

struct Metrics {
    ReshardingCumulativeMetrics _resharding;
    ReshardingCumulativeMetrics _moveCollection;
    ReshardingCumulativeMetrics _balancerMoveCollection;
    ReshardingCumulativeMetrics _unshardCollection;

    Metrics()
        : _moveCollection{"moveCollection"},
          _balancerMoveCollection{"balancerMoveCollection"},
          _unshardCollection{"unshardCollection"} {};
};
using MetricsPtr = std::unique_ptr<Metrics>;
const auto getMetrics = ServiceContext::declareDecoration<MetricsPtr>();

const auto metricsRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ShardingDataTransformMetrics", [](ServiceContext* ctx) {
        getMetrics(ctx) = std::make_unique<Metrics>();
    }};

}  // namespace

ReshardingCumulativeMetrics* ReshardingCumulativeMetrics::getForResharding(
    ServiceContext* context) {
    auto& metrics = getMetrics(context);
    return &metrics->_resharding;
}

ReshardingCumulativeMetrics* ReshardingCumulativeMetrics::getForMoveCollection(
    ServiceContext* context) {
    auto& metrics = getMetrics(context);
    return &metrics->_moveCollection;
}

ReshardingCumulativeMetrics* ReshardingCumulativeMetrics::getForBalancerMoveCollection(
    ServiceContext* context) {
    auto& metrics = getMetrics(context);
    return &metrics->_balancerMoveCollection;
}

ReshardingCumulativeMetrics* ReshardingCumulativeMetrics::getForUnshardCollection(
    ServiceContext* context) {
    auto& metrics = getMetrics(context);
    return &metrics->_unshardCollection;
}

ReshardingCumulativeMetrics::UniqueScopedObserver
ReshardingCumulativeMetrics::registerInstanceMetrics(const ReshardingMetricsObserver* metrics) {
    _shouldReportMetrics.store(true);
    auto role = metrics->getRole();
    auto it = insertMetrics(metrics, getMetricsSetForRole(role));
    return std::make_unique<ReshardingCumulativeMetrics::ScopedObserver>(this, role, std::move(it));
}

boost::optional<StringData> ReshardingCumulativeMetrics::fieldNameFor(AnyState state) {
    return StateTracker::getNameFor(state, kReportedStateFieldNamesMap);
}

ReshardingCumulativeMetrics::ReshardingCumulativeMetrics()
    : ReshardingCumulativeMetrics(kResharding) {}

ReshardingCumulativeMetrics::ReshardingCumulativeMetrics(const std::string& rootName)
    : _rootSectionName{rootName},
      _instanceMetricsForAllRoles(ReshardingMetricsCommon::kRoleCount),
      _shouldReportMetrics{false} {}


int64_t ReshardingCumulativeMetrics::getOldestOperationHighEstimateRemainingTimeMillis(
    Role role) const {
    return getOldestOperationEstimateRemainingTimeMillis(role, EstimateType::kHigh);
}

int64_t ReshardingCumulativeMetrics::getOldestOperationLowEstimateRemainingTimeMillis(
    Role role) const {
    return getOldestOperationEstimateRemainingTimeMillis(role, EstimateType::kLow);
}

int64_t ReshardingCumulativeMetrics::getOldestOperationEstimateRemainingTimeMillis(
    Role role, EstimateType type) const {

    stdx::unique_lock guard(_mutex);
    auto op = getOldestOperation(guard, role);
    if (!op) {
        return kEstimateNotAvailable;
    }
    auto estimate = getEstimate(op, type);
    return estimate ? estimate->count() : kEstimateNotAvailable;
}

boost::optional<Milliseconds> ReshardingCumulativeMetrics::getEstimate(
    const ReshardingMetricsObserver* op, EstimateType type) const {
    switch (type) {
        case kHigh:
            return op->getHighEstimateRemainingTimeMillis();
        case kLow:
            return op->getLowEstimateRemainingTimeMillis();
    }
    MONGO_UNREACHABLE;
}

size_t ReshardingCumulativeMetrics::getObservedMetricsCount() const {
    stdx::unique_lock guard(_mutex);
    size_t count = 0;
    for (const auto& set : _instanceMetricsForAllRoles) {
        count += set.size();
    }
    return count;
}

size_t ReshardingCumulativeMetrics::getObservedMetricsCount(Role role) const {
    stdx::unique_lock guard(_mutex);
    return getMetricsSetForRole(role).size();
}

void ReshardingCumulativeMetrics::reportActive(BSONObjBuilder* bob) const {
    using namespace resharding_metrics::field_names;
    bob->append(kDocumentsCopied, _documentsProcessed.load());
    bob->append(kBytesCopied, _bytesWritten.load());
    bob->append(kCountWritesToStashCollections, _writesToStashedCollections.load());
    bob->append(kCountWritesDuringCriticalSection, _writesDuringCriticalSection.load());
    bob->append(kCountReadsDuringCriticalSection, _readsDuringCriticalSection.load());
    bob->append(kOplogEntriesFetched, getOplogEntriesFetched());
    bob->append(kOplogEntriesApplied, getOplogEntriesApplied());
    bob->append(kInsertsApplied, getInsertsApplied());
    bob->append(kUpdatesApplied, getUpdatesApplied());
    bob->append(kDeletesApplied, getDeletesApplied());
}

void ReshardingCumulativeMetrics::reportLatencies(BSONObjBuilder* bob) const {
    using namespace resharding_metrics::field_names;
    bob->append(kCollectionCloningTotalRemoteBatchRetrievalTimeMillis,
                _totalBatchRetrievedDuringCloneMillis.load());
    bob->append(kCollectionCloningTotalRemoteBatchesRetrieved,
                _totalBatchRetrievedDuringClone.load());
    bob->append(kCollectionCloningTotalLocalInsertTimeMillis,
                _collectionCloningTotalLocalInsertTimeMillis.load());
    bob->append(kCollectionCloningTotalLocalInserts,
                _collectionCloningTotalLocalBatchInserts.load());
    bob->append(kOplogFetchingTotalRemoteBatchRetrievalTimeMillis,
                getOplogFetchingTotalRemoteBatchesRetrievalTimeMillis());
    bob->append(kOplogFetchingTotalRemoteBatchesRetrieved,
                getOplogFetchingTotalRemoteBatchesRetrieved());
    bob->append(kOplogFetchingTotalLocalInsertTimeMillis,
                getOplogFetchingTotalLocalInsertTimeMillis());
    bob->append(kOplogFetchingTotalLocalInserts, getOplogFetchingTotalLocalInserts());
    bob->append(kOplogApplyingTotalLocalBatchRetrievalTimeMillis,
                getOplogApplyingTotalBatchesRetrievalTimeMillis());
    bob->append(kOplogApplyingTotalLocalBatchesRetrieved, getOplogApplyingTotalBatchesRetrieved());
    bob->append(kOplogApplyingTotalLocalBatchApplyTimeMillis, getOplogBatchAppliedMillis());
    bob->append(kOplogApplyingTotalLocalBatchesApplied, getOplogBatchApplied());
}

void ReshardingCumulativeMetrics::reportCurrentInSteps(BSONObjBuilder* bob) const {
    reportCountsForAllStates(kReportedStateFieldNamesMap, bob);
}

void ReshardingCumulativeMetrics::reportForServerStatus(BSONObjBuilder* bob) const {
    using namespace resharding_metrics::field_names;

    if (!_shouldReportMetrics.load()) {
        return;
    }

    BSONObjBuilder root(bob->subobjStart(_rootSectionName));
    root.append(kCountStarted, _countStarted.load());
    root.append(kCountSucceeded, _countSucceeded.load());
    root.append(kCountFailed, _countFailed.load());
    root.append(kCountCanceled, _countCancelled.load());
    root.append(kLastOpEndingChunkImbalance, _lastOpEndingChunkImbalance.load());

    if (_rootSectionName == kResharding) {
        root.append(kCountSameKeyStarted, _countSameKeyStarted.load());
        root.append(kCountSameKeySucceeded, _countSameKeySucceeded.load());
        root.append(kCountSameKeyFailed, _countSameKeyFailed.load());
        root.append(kCountSameKeyCanceled, _countSameKeyCancelled.load());
    }

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

void ReshardingCumulativeMetrics::reportOldestActive(BSONObjBuilder* bob) const {
    using namespace resharding_metrics::field_names;
    bob->append(kCoordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis,
                getOldestOperationHighEstimateRemainingTimeMillis(Role::kCoordinator));
    bob->append(kCoordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis,
                getOldestOperationLowEstimateRemainingTimeMillis(Role::kCoordinator));
    bob->append(kRecipientRemainingOperationTimeEstimatedMillis,
                getOldestOperationHighEstimateRemainingTimeMillis(Role::kRecipient));
}

int64_t ReshardingCumulativeMetrics::getInsertsApplied() const {
    return _insertsApplied.load();
}

int64_t ReshardingCumulativeMetrics::getUpdatesApplied() const {
    return _updatesApplied.load();
}

int64_t ReshardingCumulativeMetrics::getDeletesApplied() const {
    return _deletesApplied.load();
}

int64_t ReshardingCumulativeMetrics::getOplogEntriesFetched() const {
    return _oplogEntriesFetched.load();
}

int64_t ReshardingCumulativeMetrics::getOplogEntriesApplied() const {
    return _oplogEntriesApplied.load();
}

int64_t ReshardingCumulativeMetrics::getOplogFetchingTotalRemoteBatchesRetrieved() const {
    return _oplogFetchingTotalRemoteBatchesRetrieved.load();
}

int64_t ReshardingCumulativeMetrics::getOplogFetchingTotalRemoteBatchesRetrievalTimeMillis() const {
    return _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis.load();
}

int64_t ReshardingCumulativeMetrics::getOplogFetchingTotalLocalInserts() const {
    return _oplogFetchingTotalLocalInserts.load();
}

int64_t ReshardingCumulativeMetrics::getOplogFetchingTotalLocalInsertTimeMillis() const {
    return _oplogFetchingTotalLocalInsertTimeMillis.load();
}

int64_t ReshardingCumulativeMetrics::getOplogApplyingTotalBatchesRetrieved() const {
    return _oplogApplyingTotalBatchesRetrieved.load();
}

int64_t ReshardingCumulativeMetrics::getOplogApplyingTotalBatchesRetrievalTimeMillis() const {
    return _oplogApplyingTotalBatchesRetrievalTimeMillis.load();
}

int64_t ReshardingCumulativeMetrics::getOplogBatchApplied() const {
    return _oplogBatchApplied.load();
}

int64_t ReshardingCumulativeMetrics::getOplogBatchAppliedMillis() const {
    return _oplogBatchAppliedMillis.load();
}

void ReshardingCumulativeMetrics::reportCountsForAllStates(
    const StateTracker::StateFieldNameMap& names, BSONObjBuilder* bob) const {
    _stateTracker.reportCountsForAllStates(names, bob);
}

const ReshardingMetricsObserver* ReshardingCumulativeMetrics::getOldestOperation(WithLock,
                                                                                 Role role) const {
    auto set = getMetricsSetForRole(role);
    if (set.empty()) {
        return nullptr;
    }
    return *set.begin();
}

ReshardingCumulativeMetrics::MetricsSet& ReshardingCumulativeMetrics::getMetricsSetForRole(
    Role role) {
    return _instanceMetricsForAllRoles[static_cast<size_t>(role)];
}

const ReshardingCumulativeMetrics::MetricsSet& ReshardingCumulativeMetrics::getMetricsSetForRole(
    Role role) const {
    return _instanceMetricsForAllRoles[static_cast<size_t>(role)];
}

ReshardingCumulativeMetrics::MetricsSet::iterator ReshardingCumulativeMetrics::insertMetrics(
    const ReshardingMetricsObserver* metrics, MetricsSet& set) {
    stdx::unique_lock guard(_mutex);
    auto before = set.size();
    auto it = set.insert(set.end(), metrics);
    invariant(before + 1 == set.size());
    return it;
}

void ReshardingCumulativeMetrics::deregisterMetrics(
    const Role& role, const ReshardingCumulativeMetrics::MetricsSet::iterator& metricsIterator) {
    stdx::unique_lock guard(_mutex);
    getMetricsSetForRole(role).erase(metricsIterator);
}

void ReshardingCumulativeMetrics::setLastOpEndingChunkImbalance(int64_t imbalanceCount) {
    _lastOpEndingChunkImbalance.store(imbalanceCount);
}

void ReshardingCumulativeMetrics::onInsertsDuringCloning(int64_t count,
                                                         int64_t bytes,
                                                         const Milliseconds& elapsedTime) {
    _collectionCloningTotalLocalBatchInserts.fetchAndAdd(1);
    _documentsProcessed.fetchAndAdd(count);
    _bytesWritten.fetchAndAdd(bytes);
    _collectionCloningTotalLocalInsertTimeMillis.fetchAndAdd(
        durationCount<Milliseconds>(elapsedTime));
}

void ReshardingCumulativeMetrics::onInsertApplied() {
    _insertsApplied.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onUpdateApplied() {
    _updatesApplied.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onDeleteApplied() {
    _deletesApplied.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onOplogEntriesFetched(int64_t numEntries) {
    _oplogEntriesFetched.fetchAndAdd(numEntries);
}

void ReshardingCumulativeMetrics::onOplogEntriesApplied(int64_t numEntries) {
    _oplogEntriesApplied.fetchAndAdd(numEntries);
}

void ReshardingCumulativeMetrics::onBatchRetrievedDuringOplogFetching(Milliseconds elapsed) {
    _oplogFetchingTotalRemoteBatchesRetrieved.fetchAndAdd(1);
    _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis.fetchAndAdd(
        durationCount<Milliseconds>(elapsed));
}

void ReshardingCumulativeMetrics::onLocalInsertDuringOplogFetching(
    const Milliseconds& elapsedTime) {
    _oplogFetchingTotalLocalInserts.fetchAndAdd(1);
    _oplogFetchingTotalLocalInsertTimeMillis.fetchAndAdd(durationCount<Milliseconds>(elapsedTime));
}

void ReshardingCumulativeMetrics::onBatchRetrievedDuringOplogApplying(
    const Milliseconds& elapsedTime) {
    _oplogApplyingTotalBatchesRetrieved.fetchAndAdd(1);
    _oplogApplyingTotalBatchesRetrievalTimeMillis.fetchAndAdd(
        durationCount<Milliseconds>(elapsedTime));
}

void ReshardingCumulativeMetrics::onOplogLocalBatchApplied(Milliseconds elapsed) {
    _oplogBatchApplied.fetchAndAdd(1);
    _oplogBatchAppliedMillis.fetchAndAdd(durationCount<Milliseconds>(elapsed));
}

void ReshardingCumulativeMetrics::onReadDuringCriticalSection() {
    _readsDuringCriticalSection.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onWriteDuringCriticalSection() {
    _writesDuringCriticalSection.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onWriteToStashedCollections() {
    _writesToStashedCollections.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onCloningRemoteBatchRetrieval(Milliseconds elapsed) {
    _totalBatchRetrievedDuringClone.fetchAndAdd(1);
    _totalBatchRetrievedDuringCloneMillis.fetchAndAdd(durationCount<Milliseconds>(elapsed));
}

ReshardingCumulativeMetrics::ScopedObserver::ScopedObserver(ReshardingCumulativeMetrics* metrics,
                                                            Role role,
                                                            MetricsSet::iterator observerIterator)
    : _metrics(metrics), _role(role), _observerIterator(std::move(observerIterator)) {}

ReshardingCumulativeMetrics::ScopedObserver::~ScopedObserver() {
    _metrics->deregisterMetrics(_role, _observerIterator);
}

void ReshardingCumulativeMetrics::onStarted(bool isSameKeyResharding, const UUID& reshardingUUID) {
    {
        stdx::lock_guard<stdx::mutex> lk(_activeReshardingOperationsMutex);
        if (_activeReshardingOperations.contains(reshardingUUID)) {
            return;
        }
        _activeReshardingOperations.insert(reshardingUUID);
    }

    if (_rootSectionName == kResharding && isSameKeyResharding) {
        _countSameKeyStarted.fetchAndAdd(1);
    }
    _countStarted.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onSuccess(bool isSameKeyResharding, const UUID& reshardingUUID) {
    {
        stdx::lock_guard<stdx::mutex> lk(_activeReshardingOperationsMutex);
        if (!_activeReshardingOperations.contains(reshardingUUID)) {
            return;
        }
        _activeReshardingOperations.erase(reshardingUUID);
    }

    if (_rootSectionName == kResharding && isSameKeyResharding) {
        _countSameKeySucceeded.fetchAndAdd(1);
    }
    _countSucceeded.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onFailure(bool isSameKeyResharding, const UUID& reshardingUUID) {
    {
        stdx::lock_guard<stdx::mutex> lk(_activeReshardingOperationsMutex);
        if (!_activeReshardingOperations.contains(reshardingUUID)) {
            return;
        }
        _activeReshardingOperations.erase(reshardingUUID);
    }

    if (_rootSectionName == kResharding && isSameKeyResharding) {
        _countSameKeyFailed.fetchAndAdd(1);
    }
    _countFailed.fetchAndAdd(1);
}

void ReshardingCumulativeMetrics::onCanceled(bool isSameKeyResharding, const UUID& reshardingUUID) {
    {
        stdx::lock_guard<stdx::mutex> lk(_activeReshardingOperationsMutex);
        if (!_activeReshardingOperations.contains(reshardingUUID)) {
            return;
        }
        _activeReshardingOperations.erase(reshardingUUID);
    }

    if (_rootSectionName == kResharding && isSameKeyResharding) {
        _countSameKeyCancelled.fetchAndAdd(1);
    }
    _countCancelled.fetchAndAdd(1);
}

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <absl/container/node_hash_map.h>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/index_builds/active_index_builds.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

auto& activeIndexBuildsGauge = otel::metrics::MetricsService::instance().createInt64Gauge(
    otel::metrics::MetricNames::kIndexBuildsActive,
    "Number of index builds currently in progress",
    otel::metrics::MetricUnit::kOperations);

auto& succeededIndexBuildsCounter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kIndexBuildsSucceeded,
    "Number of index builds that completed successfully, including no-op completions",
    otel::metrics::MetricUnit::kOperations);

auto& failedIndexBuildsCounter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kIndexBuildsFailed,
    "Number of index builds that did not complete successfully",
    otel::metrics::MetricUnit::kOperations);

auto& toBeResumedIndexBuildsCounter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kIndexBuildsToBeResumed,
    "Number of index builds that were interrupted and will be resumed",
    otel::metrics::MetricUnit::kOperations);

auto& startedIndexBuildsCounter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kIndexBuildsStarted,
    "Number of index builds started",
    otel::metrics::MetricUnit::kOperations);

auto& resumeSucceededCounter =
    otel::metrics::MetricsService::instance().createInt64Counter<std::string_view>(
        otel::metrics::MetricNames::kIndexBuildResumeSucceeded,
        "Number of primary-driven index builds successfully resumed",
        otel::metrics::MetricUnit::kOperations,
        otel::metrics::AttributeDefinition<std::string_view>{
            .name = "phase",
            .values = {
                idl::serialize(IndexBuildPhaseEnum::kInitialized),
                idl::serialize(IndexBuildPhaseEnum::kCollectionScan),
                idl::serialize(IndexBuildPhaseEnum::kBulkLoad),
                idl::serialize(IndexBuildPhaseEnum::kDrainWrites),
            }});

auto& resumeFailedCounter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kIndexBuildResumeFailed,
    "Number of primary-driven index builds that failed to resume",
    otel::metrics::MetricUnit::kOperations);

void recordIndexBuildOutcome(IndexBuildOutcome outcome) {
    switch (outcome) {
        case IndexBuildOutcome::kSuccess:
            succeededIndexBuildsCounter.add(1);
            return;
        case IndexBuildOutcome::kFailure:
            failedIndexBuildsCounter.add(1);
            return;
        case IndexBuildOutcome::kToBeResumed:
            toBeResumedIndexBuildsCounter.add(1);
            return;
    }
    MONGO_UNREACHABLE;
}

}  // namespace

ActiveIndexBuilds::~ActiveIndexBuilds() {
    invariant(_allIndexBuilds.empty());
}

void ActiveIndexBuilds::waitForAllIndexBuildsToStopForShutdown() {
    waitForAllIndexBuildsToStop(OperationContext::notInterruptible());
}

void ActiveIndexBuilds::waitForAllIndexBuildsToStop(Interruptible* interruptible) {
    std::unique_lock<std::mutex> lk(_mutex);

    // All index builds should have been signaled to stop via the ServiceContext.

    if (_allIndexBuilds.empty()) {
        return;
    }

    auto indexBuildToUUID = [](const auto& indexBuild) {
        return indexBuild.first;
    };
    auto begin = boost::make_transform_iterator(_allIndexBuilds.begin(), indexBuildToUUID);
    auto end = boost::make_transform_iterator(_allIndexBuilds.end(), indexBuildToUUID);
    LOGV2(4725201,
          "Waiting until the following index builds are finished",
          "indexBuilds"_attr = logv2::seqLog(begin, end));

    // Wait for all the index builds to stop.
    auto pred = [this]() {
        return _allIndexBuilds.empty();
    };
    interruptible->waitForConditionOrInterrupt(_indexBuildsCondVar, lk, pred);
}

void ActiveIndexBuilds::assertNoIndexBuildInProgress() const {
    std::unique_lock<std::mutex> lk(_mutex);
    if (!_allIndexBuilds.empty()) {
        auto firstIndexBuild = _allIndexBuilds.cbegin()->second;
        uasserted(ErrorCodes::BackgroundOperationInProgressForDatabase,
                  fmt::format("cannot perform operation: there are currently {} index builds "
                              "running. Found index build: {}",
                              _allIndexBuilds.size(),
                              firstIndexBuild->buildUUID.toString()));
    }
}

void ActiveIndexBuilds::waitUntilAnIndexBuildFinishes(OperationContext* opCtx, Date_t deadline) {
    std::unique_lock<std::mutex> lk(_mutex);
    if (_allIndexBuilds.empty()) {
        return;
    }
    const auto generation = _indexBuildsCompletedGen;
    opCtx->waitForConditionOrInterruptUntil(
        _indexBuildsCondVar, lk, deadline, [&] { return _indexBuildsCompletedGen != generation; });
}

void ActiveIndexBuilds::sleepIndexBuilds_forTestOnly(bool sleep) {
    std::unique_lock<std::mutex> lk(_mutex);
    _sleepForTest = sleep;
}

void ActiveIndexBuilds::verifyNoIndexBuilds_forTestOnly() const {
    std::unique_lock<std::mutex> lk(_mutex);
    invariant(_allIndexBuilds.empty());
}

void ActiveIndexBuilds::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                 const UUID& collectionUUID,
                                                                 IndexBuildProtocol protocol) {
    std::unique_lock<std::mutex> lk(_mutex);
    auto noIndexBuildsPred = [&, this]() {
        auto indexBuilds = _filterIndexBuilds_inlock(lk, [&](const auto& replState) {
            return collectionUUID == replState.collectionUUID && protocol == replState.protocol;
        });
        return indexBuilds.empty();
    };
    opCtx->waitForConditionOrInterrupt(_indexBuildsCondVar, lk, noIndexBuildsPred);
}

void ActiveIndexBuilds::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                 const UUID& collectionUUID) {
    std::unique_lock<std::mutex> lk(_mutex);
    auto pred = [&, this]() {
        auto indexBuilds = _filterIndexBuilds_inlock(
            lk, [&](const auto& replState) { return collectionUUID == replState.collectionUUID; });
        return indexBuilds.empty();
    };
    _indexBuildsCondVar.wait(lk, pred);
}

StatusWith<std::shared_ptr<ReplIndexBuildState>> ActiveIndexBuilds::getIndexBuild(
    const UUID& buildUUID) const {
    std::unique_lock<std::mutex> lk(_mutex);
    auto it = _allIndexBuilds.find(buildUUID);
    if (it == _allIndexBuilds.end()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "No index build with UUID: " << buildUUID};
    }
    return it->second;
}

std::vector<std::shared_ptr<ReplIndexBuildState>> ActiveIndexBuilds::getAllIndexBuilds() const {
    std::unique_lock<std::mutex> lk(_mutex);
    return _filterIndexBuilds_inlock(lk, [](const auto& replState) { return true; });
}

void ActiveIndexBuilds::unregisterIndexBuild(
    IndexBuildsManager* indexBuildsManager,
    std::shared_ptr<ReplIndexBuildState> replIndexBuildState,
    IndexBuildOutcome outcome) {

    std::unique_lock<std::mutex> lk(_mutex);

    invariant(_allIndexBuilds.erase(replIndexBuildState->buildUUID));

    LOGV2_DEBUG(4656004,
                1,
                "Index build: unregistering",
                "buildUUID"_attr = replIndexBuildState->buildUUID,
                "collectionUUID"_attr = replIndexBuildState->collectionUUID);

    recordIndexBuildOutcome(outcome);
    activeIndexBuildsGauge.set(_allIndexBuilds.size());
    indexBuildsManager->tearDownAndUnregisterIndexBuild(replIndexBuildState->buildUUID);
    _indexBuildsCompletedGen++;
    _indexBuildsCondVar.notify_all();
}

void ActiveIndexBuilds::incrementResumeSucceeded(IndexBuildPhaseEnum phase) {
    resumeSucceededCounter.add(1, {idl::serialize(phase)});
}

void ActiveIndexBuilds::incrementResumeFailed() {
    resumeFailedCounter.add(1);
}

std::vector<std::shared_ptr<ReplIndexBuildState>> ActiveIndexBuilds::filterIndexBuilds(
    IndexBuildFilterFn indexBuildFilter) const {

    std::unique_lock<std::mutex> lk(_mutex);
    return _filterIndexBuilds_inlock(lk, indexBuildFilter);
}

std::vector<std::shared_ptr<ReplIndexBuildState>> ActiveIndexBuilds::_filterIndexBuilds_inlock(
    WithLock lk, IndexBuildFilterFn indexBuildFilter) const {

    std::vector<std::shared_ptr<ReplIndexBuildState>> indexBuilds;
    for (const auto& pair : _allIndexBuilds) {
        auto replState = pair.second;
        if (!indexBuildFilter(*replState)) {
            continue;
        }
        indexBuilds.push_back(replState);
    }
    return indexBuilds;
}

void ActiveIndexBuilds::awaitNoBgOpInProgForDb(OperationContext* opCtx,
                                               const DatabaseName& dbName) {
    std::unique_lock<std::mutex> lk(_mutex);
    auto indexBuildFilter = [dbName](const auto& replState) {
        return dbName == replState.dbName;
    };
    auto pred = [&, this]() {
        auto dbIndexBuilds = _filterIndexBuilds_inlock(lk, indexBuildFilter);
        return dbIndexBuilds.empty();
    };
    _indexBuildsCondVar.wait(lk, pred);
}

Status ActiveIndexBuilds::registerIndexBuild(
    std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
    std::unique_lock<std::mutex> lk(_mutex);
    // Check whether any indexes are already being built with the same index name(s). (Duplicate
    // specs will be discovered by the index builder.)
    auto pred = [&](const auto& replState) {
        return replIndexBuildState->collectionUUID == replState.collectionUUID;
    };
    auto collIndexBuilds = _filterIndexBuilds_inlock(lk, pred);
    for (const auto& existingIndexBuild : collIndexBuilds) {
        for (const auto& name : toIndexNames(replIndexBuildState->getIndexes())) {
            auto existingIndexNames = toIndexNames(existingIndexBuild->getIndexes());
            if (existingIndexNames.end() !=
                std::find(existingIndexNames.begin(), existingIndexNames.end(), name)) {
                return existingIndexBuild->onConflictWithNewIndexBuild(*replIndexBuildState, name);
            }
        }
    }

    invariant(_allIndexBuilds.emplace(replIndexBuildState->buildUUID, replIndexBuildState).second);

    activeIndexBuildsGauge.set(_allIndexBuilds.size());
    startedIndexBuildsCounter.add(1);
    _indexBuildsCondVar.notify_all();

    return Status::OK();
}

size_t ActiveIndexBuilds::getActiveIndexBuildsCount() const {
    std::unique_lock<std::mutex> lk(_mutex);
    return _allIndexBuilds.size();
}

void ActiveIndexBuilds::appendBuildInfo(const UUID& buildUUID, BSONObjBuilder* builder) const {
    std::unique_lock<std::mutex> lk(_mutex);
    auto it = _allIndexBuilds.find(buildUUID);
    if (it == _allIndexBuilds.end()) {
        return;
    }
    it->second->appendBuildInfo(builder);
}

void ActiveIndexBuilds::sleepIfNecessary_forTestOnly() const {
    std::unique_lock<std::mutex> lk(_mutex);
    while (_sleepForTest) {
        lk.unlock();
        sleepmillis(100);
        lk.lock();
    }
}
}  // namespace mongo

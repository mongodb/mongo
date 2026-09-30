// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/active_index_builds.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

std::string_view toString(IndexBuildOutcome outcome) {
    switch (outcome) {
        case IndexBuildOutcome::kSuccess:
            return "success";
        case IndexBuildOutcome::kFailure:
            return "failure";
        case IndexBuildOutcome::kToBeResumed:
            return "to_be_resumed";
    }
    MONGO_UNREACHABLE;
}

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

// The histogram buckets for `kIndexBuildCompletedDurationMillis`. Index builds range from
// milliseconds for a no-op completion to days for a large collection.
const std::vector<double> kIndexBuildDurationBucketsMillis = {
    10,           // 10ms
    1'000,        // 1s
    10'000,       // 10s
    60'000,       // 1min
    300'000,      // 5min
    1'800'000,    // 30min
    7'200'000,    // 2h
    28'800'000,   // 8h
    86'400'000,   // 1d
    172'800'000,  // 2d
    259'200'000,  // 3d
    432'000'000,  // 5d
};

// The histogram buckets for `kIndexBuildCompletedKeysWritten`, log scaled in steps of 10x.
const std::vector<double> kIndexBuildKeysWrittenBuckets = {
    0,               // wrote no keys
    10,              // 10
    100,             // 100
    1'000,           // 1k
    10'000,          // 10k
    100'000,         // 100k
    1'000'000,       // 1M
    10'000'000,      // 10M
    100'000'000,     // 100M
    1'000'000'000,   // 1B
    10'000'000'000,  // 10B
};

// The histogram buckets for `kIndexBuildCompletedBytesWritten`. Log scaled in steps of 8x, from a
// single small key up to multi-terabyte builds.
const std::vector<double> kIndexBuildBytesWrittenBuckets = {
    0,                  // wrote no bytes
    1'024,              // 1 KiB
    8'192,              // 8 KiB
    65'536,             // 64 KiB
    524'288,            // 512 KiB
    4'194'304,          // 4 MiB
    33'554'432,         // 32 MiB
    268'435'456,        // 256 MiB
    2'147'483'648,      // 2 GiB
    17'179'869'184,     // 16 GiB
    137'438'953'472,    // 128 GiB
    1'099'511'627'776,  // 1 TiB
};

otel::metrics::AttributeDefinition<std::string_view> makeStartPhaseAttribute() {
    return {.name = "start_phase",
            .values = {
                idl::serialize(IndexBuildPhaseEnum::kInitialized),
                idl::serialize(IndexBuildPhaseEnum::kCollectionScan),
                idl::serialize(IndexBuildPhaseEnum::kBulkLoad),
                idl::serialize(IndexBuildPhaseEnum::kDrainWrites),
            }};
}

otel::metrics::AttributeDefinition<std::string_view> makeOutcomeAttribute() {
    return {.name = "outcome",
            .values = {
                toString(IndexBuildOutcome::kSuccess),
                toString(IndexBuildOutcome::kFailure),
                toString(IndexBuildOutcome::kToBeResumed),
            }};
}

auto& indexBuildsCompletedDurationMillisHistogram =
    otel::metrics::MetricsService::instance()
        .createInt64Histogram<std::string_view, std::string_view>(
            otel::metrics::MetricNames::kIndexBuildCompletedDurationMillis,
            "Duration of index builds on this node, by the phase they were "
            "started or resumed from and by their outcome",
            otel::metrics::MetricUnit::kMilliseconds,
            makeStartPhaseAttribute(),
            makeOutcomeAttribute(),
            {.explicitBucketBoundaries = kIndexBuildDurationBucketsMillis});

auto& indexBuildsCompletedKeysWrittenHistogram =
    otel::metrics::MetricsService::instance()
        .createInt64Histogram<std::string_view, std::string_view>(
            otel::metrics::MetricNames::kIndexBuildCompletedKeysWritten,
            "Number of index keys written to the index tables by index builds on this node, "
            "including deletions, by the phase they were started or resumed from and by their "
            "outcome",
            otel::metrics::MetricUnit::kCount,
            makeStartPhaseAttribute(),
            makeOutcomeAttribute(),
            {.explicitBucketBoundaries = kIndexBuildKeysWrittenBuckets});

auto& indexBuildsCompletedBytesWrittenHistogram =
    otel::metrics::MetricsService::instance()
        .createInt64Histogram<std::string_view, std::string_view>(
            otel::metrics::MetricNames::kIndexBuildCompletedBytesWritten,
            "Number of key string bytes written to the index tables by index builds on this "
            "node, including deletions, by the phase they were started or resumed from and by "
            "their outcome",
            otel::metrics::MetricUnit::kBytes,
            makeStartPhaseAttribute(),
            makeOutcomeAttribute(),
            {.explicitBucketBoundaries = kIndexBuildBytesWrittenBuckets});

bool includesPrimaryDriven(std::initializer_list<IndexBuildProtocol> protocols) {
    return std::find(protocols.begin(), protocols.end(), IndexBuildProtocol::kPrimaryDriven) !=
        protocols.end();
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

    auto noneRegistered = [this]() {
        return !_primaryDrivenRegistry || _primaryDrivenRegistry->all().empty();
    };

    if (_allIndexBuilds.empty() && noneRegistered()) {
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
    auto pred = [&, this]() {
        return _allIndexBuilds.empty() && noneRegistered();
    };
    interruptible->waitForConditionOrInterrupt(_indexBuildsCondVar, lk, pred);
}

void ActiveIndexBuilds::assertNoIndexBuildInProgress() const {
    std::unique_lock<std::mutex> lk(_mutex);
    auto matching = _filterIndexBuilds_inlock(lk, [](const auto& replState) { return true; });
    uassert(ErrorCodes::BackgroundOperationInProgressForDatabase,
            fmt::format("cannot perform operation: there are currently {} index builds "
                        "running. Found index build: {}",
                        matching.size(),
                        matching.front()->buildUUID.toString()),
            matching.empty());

    if (!_primaryDrivenRegistry) {
        return;
    }
    auto registered = _primaryDrivenRegistry->all();
    uassert(ErrorCodes::BackgroundOperationInProgressForDatabase,
            fmt::format("cannot perform operation: there are currently {} primary-driven "
                        "index builds registered. Found index build: {}",
                        registered.size(),
                        registered.front().first.toString()),
            registered.empty());
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

void ActiveIndexBuilds::_awaitNoIndexBuildInProgressForFilter(OperationContext* opCtx,
                                                              IndexBuildFilterFn indexBuildFilter) {
    std::unique_lock<std::mutex> lk(_mutex);
    auto noIndexBuildsPred = [&, this]() {
        auto indexBuilds = _filterIndexBuilds_inlock(lk, indexBuildFilter);
        return indexBuilds.empty();
    };
    opCtx->waitForConditionOrInterrupt(_indexBuildsCondVar, lk, noIndexBuildsPred);
}

void ActiveIndexBuilds::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                 const UUID& collectionUUID,
                                                                 IndexBuildProtocol protocol) {
    _awaitNoIndexBuildInProgressForFilters(
        opCtx,
        [&](const auto& replState) {
            return collectionUUID == replState.collectionUUID && protocol == replState.protocol;
        },
        // Every build in the registry is primary-driven.
        [&](const auto& build) {
            return protocol == IndexBuildProtocol::kPrimaryDriven &&
                collectionUUID == build.collectionUUID;
        });
}

void ActiveIndexBuilds::_awaitNoIndexBuildInProgressForFilters(
    OperationContext* opCtx,
    IndexBuildFilterFn runningFilter,
    std::function<bool(const index_builds::primary_driven::Registry::Entry&)> registeredFilter) {
    std::unique_lock<std::mutex> lk(_mutex);
    auto noIndexBuildsPred = [&, this]() {
        if (!_filterIndexBuilds_inlock(lk, runningFilter).empty()) {
            return false;
        }
        if (!_primaryDrivenRegistry) {
            return true;
        }
        for (auto&& [buildUUID, build] : _primaryDrivenRegistry->all()) {
            if (registeredFilter(build)) {
                return false;
            }
        }
        return true;
    };
    opCtx->waitForConditionOrInterrupt(_indexBuildsCondVar, lk, noIndexBuildsPred);
}

void ActiveIndexBuilds::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                 const UUID& collectionUUID) {
    _awaitNoIndexBuildInProgressForFilters(
        opCtx,
        [&](const auto& replState) { return collectionUUID == replState.collectionUUID; },
        [&](const auto& build) { return collectionUUID == build.collectionUUID; });
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

    const auto metrics = replIndexBuildState->getIndexBuildMetrics();
    // The phase that the index build was in when we unregistered it, and the number of keys and
    // amount of data it wrote to index tables. If there are no index builds with this build UUID
    // present (i.e, if we registered it as an active index build but did not successfully set it
    // up), fall back to reporting kInitialized as the endPhase, and leave the write stats unset.
    const auto endPhase = indexBuildsManager->getPhase(replIndexBuildState->buildUUID)
                              .value_or(IndexBuildPhaseEnum::kInitialized);
    const auto writeStats =
        indexBuildsManager->getNumKeysAndBytesWritten(replIndexBuildState->buildUUID);
    const int64_t durationMillis =
        std::max(int64_t{0}, (Date_t::now() - metrics.startTime).count());

    LOGV2(4656004,
          "Index build: completed",
          "buildUUID"_attr = replIndexBuildState->buildUUID,
          "collectionUUID"_attr = replIndexBuildState->collectionUUID,
          "outcome"_attr = toString(outcome),
          "startPhase"_attr = idl::serialize(metrics.startPhase),
          "endPhase"_attr = idl::serialize(endPhase),
          "durationMillis"_attr = durationMillis,
          "numKeysWrittenBulkLoad"_attr = writeStats ? writeStats->numKeysWrittenBulkLoad : 0,
          "numBytesWrittenBulkLoad"_attr = writeStats ? writeStats->numBytesWrittenBulkLoad : 0,
          "numKeysWrittenSideWritesDrain"_attr =
              writeStats ? writeStats->numKeysWrittenSideWritesDrain : 0,
          "numBytesWrittenSideWritesDrain"_attr =
              writeStats ? writeStats->numBytesWrittenSideWritesDrain : 0);

    recordIndexBuildOutcome(outcome);
    // A build that was never set up did no index build work, so it is left out of the completion
    // histograms rather than recorded as a zero.
    if (writeStats) {
        const auto histogramAttrs =
            std::tuple{idl::serialize(metrics.startPhase), toString(outcome)};
        indexBuildsCompletedDurationMillisHistogram.record(durationMillis, histogramAttrs);
        indexBuildsCompletedKeysWrittenHistogram.record(
            writeStats->numKeysWrittenBulkLoad + writeStats->numKeysWrittenSideWritesDrain,
            histogramAttrs);
        indexBuildsCompletedBytesWrittenHistogram.record(
            writeStats->numBytesWrittenBulkLoad + writeStats->numBytesWrittenSideWritesDrain,
            histogramAttrs);
    }
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

void ActiveIndexBuilds::setPrimaryDrivenRegistry(index_builds::primary_driven::Registry& registry) {
    {
        std::unique_lock<std::mutex> lk(_mutex);
        _primaryDrivenRegistry = &registry;
    }
    registry.setOnChangeHandler([this] {
        std::lock_guard<std::mutex> lk{_mutex};
        _indexBuildsCondVar.notify_all();
    });
}

std::vector<UUID> ActiveIndexBuilds::buildUUIDsForCollection(const UUID& collectionUUID) const {
    return _buildUUIDs(
        [&](const auto& replState) { return collectionUUID == replState.collectionUUID; },
        [&](const auto& build) { return collectionUUID == build.collectionUUID; });
}

std::vector<UUID> ActiveIndexBuilds::buildUUIDsForCollection(const UUID& collectionUUID,
                                                             IndexBuildProtocol protocol) const {
    return _buildUUIDs(
        [&](const auto& replState) {
            return collectionUUID == replState.collectionUUID && protocol == replState.protocol;
        },
        [&](const auto& build) {
            return protocol == IndexBuildProtocol::kPrimaryDriven &&
                collectionUUID == build.collectionUUID;
        });
}

std::vector<UUID> ActiveIndexBuilds::buildUUIDsForDb(const DatabaseName& dbName) const {
    return _buildUUIDs([&](const auto& replState) { return dbName == replState.dbName; },
                       [&](const auto& build) { return dbName == build.dbName; });
}

std::vector<UUID> ActiveIndexBuilds::_buildUUIDs(
    const IndexBuildFilterFn& runningFilter,
    const std::function<bool(const index_builds::primary_driven::Registry::Entry&)>&
        registeredFilter) const {
    std::vector<UUID> buildUUIDs;
    index_builds::primary_driven::Registry* registry = nullptr;
    {
        std::unique_lock<std::mutex> lk(_mutex);
        for (const auto& replState : _filterIndexBuilds_inlock(lk, runningFilter)) {
            buildUUIDs.push_back(replState->buildUUID);
        }
        registry = _primaryDrivenRegistry;
    }

    if (!registry) {
        return buildUUIDs;
    }

    // A primary-driven build is registered for as long as it exists on this node, so the registry
    // also holds the builds we are running and which are already accounted for above.
    for (auto&& [buildUUID, build] : registry->all()) {
        if (registeredFilter(build) &&
            std::find(buildUUIDs.begin(), buildUUIDs.end(), buildUUID) == buildUUIDs.end()) {
            buildUUIDs.push_back(buildUUID);
        }
    }
    return buildUUIDs;
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

void ActiveIndexBuilds::awaitNoBgOpInProgForDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    std::initializer_list<IndexBuildProtocol> protocols) {
    const bool withPrimaryDriven = includesPrimaryDriven(protocols);
    _awaitNoIndexBuildInProgressForFilters(
        opCtx,
        [&](const auto& replState) {
            return dbName == replState.dbName &&
                std::find(protocols.begin(), protocols.end(), replState.protocol) !=
                protocols.end();
        },
        [&](const auto& build) { return withPrimaryDriven && dbName == build.dbName; });
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

size_t ActiveIndexBuilds::getIndexBuildsCount() const {
    return _buildUUIDs([](const auto&) { return true; }, [](const auto&) { return true; }).size();
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

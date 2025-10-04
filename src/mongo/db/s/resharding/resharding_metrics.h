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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/metrics/metrics_state_holder.h"
#include "mongo/db/s/metrics/phase_duration_tracker.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <variant>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace resharding_metrics {

enum TimedPhase { kCloning, kApplying, kCriticalSection, kBuildingIndex };
constexpr auto kNumTimedPhase = 4;
using PhaseDurationTracker = PhaseDurationTracker<TimedPhase, kNumTimedPhase>;

}  // namespace resharding_metrics

class ReshardingMetrics {
public:
    using State = ReshardingCumulativeMetrics::AnyState;
    using TimedPhase = resharding_metrics::TimedPhase;
    using TimedPhaseNameMap = resharding_metrics::PhaseDurationTracker::TimedPhaseNameMap;
    using Role = ReshardingMetricsCommon::Role;
    using ObserverPtr = std::unique_ptr<ReshardingMetricsObserver>;

    /**
     * To be used by recipients only. Tracks the exponential moving average of the time it takes for
     * a recipient to fetch an oplog entry from a donor and apply an oplog entry after it has been
     * fetched.
     */
    struct OplogLatencyMetrics {
    public:
        OplogLatencyMetrics(ShardId donorShardId);

        void updateAverageTimeToFetch(Milliseconds timeToFetch);
        void updateAverageTimeToApply(Milliseconds timeToApply);

        boost::optional<Milliseconds> getAverageTimeToFetch() const;
        boost::optional<Milliseconds> getAverageTimeToApply() const;

    private:
        const ShardId _donorShardId;

        mutable stdx::mutex _timeToFetchMutex;
        boost::optional<Milliseconds> _avgTimeToFetch;

        mutable stdx::mutex _timeToApplyMutex;
        boost::optional<Milliseconds> _avgTimeToApply;
    };

    struct ExternallyTrackedRecipientFields {
    public:
        void accumulateFrom(const ReshardingOplogApplierProgress& progressDoc);

        boost::optional<int64_t> documentCountCopied;
        boost::optional<int64_t> documentBytesCopied;
        boost::optional<int64_t> oplogEntriesFetched;
        boost::optional<int64_t> oplogEntriesApplied;
        boost::optional<int64_t> insertsApplied;
        boost::optional<int64_t> updatesApplied;
        boost::optional<int64_t> deletesApplied;
        boost::optional<int64_t> writesToStashCollections;
    };

    ReshardingMetrics(const CommonReshardingMetadata& metadata,
                      Role role,
                      ClockSource* clockSource,
                      ReshardingCumulativeMetrics* cumulativeMetrics);

    ReshardingMetrics(const CommonReshardingMetadata& metadata,
                      Role role,
                      ClockSource* clockSource,
                      ReshardingCumulativeMetrics* cumulativeMetrics,
                      State state);

    ReshardingMetrics(UUID instanceId,
                      BSONObj shardKey,
                      NamespaceString nss,
                      Role role,
                      Date_t startTime,
                      ClockSource* clockSource,
                      ReshardingCumulativeMetrics* cumulativeMetrics,
                      State state,
                      ReshardingProvenanceEnum provenance);

    ReshardingMetrics(UUID instanceId,
                      BSONObj shardKey,
                      NamespaceString nss,
                      Role role,
                      Date_t startTime,
                      ClockSource* clockSource,
                      ReshardingCumulativeMetrics* cumulativeMetrics,
                      State state,
                      ObserverPtr observer,
                      ReshardingProvenanceEnum provenance);

    ~ReshardingMetrics();

    static std::unique_ptr<ReshardingMetrics> makeInstance_forTest(UUID instanceId,
                                                                   BSONObj shardKey,
                                                                   NamespaceString nss,
                                                                   Role role,
                                                                   Date_t startTime,
                                                                   ServiceContext* serviceContext);

    template <typename T>
    static auto initializeFrom(const T& document,
                               ClockSource* clockSource,
                               ReshardingCumulativeMetrics* cumulativeMetrics) {
        static_assert(resharding_metrics::isStateDocument<T>);
        auto result =
            std::make_unique<ReshardingMetrics>(document.getCommonReshardingMetadata(),
                                                resharding_metrics::getRoleForStateDocument<T>(),
                                                clockSource,
                                                cumulativeMetrics,
                                                resharding_metrics::getState(document));
        result->restoreRoleSpecificFields(document);
        return result;
    }

    template <typename T>
    static auto initializeFrom(const T& document, ServiceContext* serviceContext) {
        auto cumulativeMetrics = [&] {
            auto provenance = document.getCommonReshardingMetadata().getProvenance().value_or(
                ReshardingProvenanceEnum::kReshardCollection);
            switch (provenance) {
                case ReshardingProvenanceEnum::kMoveCollection:
                    return ReshardingCumulativeMetrics::getForMoveCollection(serviceContext);
                case ReshardingProvenanceEnum::kBalancerMoveCollection:
                    return ReshardingCumulativeMetrics::getForBalancerMoveCollection(
                        serviceContext);
                case ReshardingProvenanceEnum::kUnshardCollection:
                    return ReshardingCumulativeMetrics::getForUnshardCollection(serviceContext);
                case ReshardingProvenanceEnum::kReshardCollection:
                    return ReshardingCumulativeMetrics::getForResharding(serviceContext);
            }
            MONGO_UNREACHABLE;
        }();

        return initializeFrom(document, serviceContext->getFastClockSource(), cumulativeMetrics);
    }

    static State getDefaultState(Role role);

    BSONObj reportForCurrentOp() const;

    enum class CalculationLogOption { Hide, Show };

    boost::optional<Milliseconds> getHighEstimateRemainingTimeMillis(
        CalculationLogOption logOption = CalculationLogOption::Hide) const;
    boost::optional<Milliseconds> getLowEstimateRemainingTimeMillis() const;
    Date_t getStartTimestamp() const;
    const UUID& getInstanceId() const;

    void onDocumentsProcessed(int64_t documentCount,
                              int64_t totalDocumentsSizeBytes,
                              Milliseconds elapsed);
    int64_t getDocumentsProcessedCount() const;
    int64_t getBytesWrittenCount() const;
    int64_t getApproxBytesToScanCount() const;
    int64_t getWritesDuringCriticalSection() const;

    void setDocumentsToProcessCounts(int64_t documentCount, int64_t totalDocumentsSizeBytes);
    void setCoordinatorHighEstimateRemainingTimeMillis(Milliseconds milliseconds);
    void setCoordinatorLowEstimateRemainingTimeMillis(Milliseconds milliseconds);

    void onCloningRemoteBatchRetrieval(Milliseconds elapsed);
    void onWriteToStashedCollections();

    void onReadDuringCriticalSection();
    void onWriteDuringCriticalSection();

    Role getRole() const;
    Seconds getOperationRunningTimeSecs() const;

    void setLastOpEndingChunkImbalance(int64_t imbalanceCount);

    ReshardingCumulativeMetrics::AnyState getState() const {
        return _state.load();
    }

    template <typename T>
    void onStateTransition(T before, boost::none_t after) {
        getCumulativeMetrics()->template onStateTransition<T>(before, boost::none);
    }

    template <typename T>
    void onStateTransition(boost::none_t before, T after) {
        setState(after);
        getCumulativeMetrics()->template onStateTransition<T>(boost::none, after);
    }

    template <typename T>
    void onStateTransition(T before, T after) {
        setState(after);
        getCumulativeMetrics()->template onStateTransition<T>(before, after);
    }

    void onInsertApplied();
    void onUpdateApplied();
    void onDeleteApplied();
    void onOplogEntriesFetched(int64_t numEntries);
    void onOplogEntriesApplied(int64_t numEntries);

    /**
     * To be used by recipients only. Registers the donors with the given shard ids. The donor
     * registration must be done exactly once and before the oplog fetchers and appliers start.
     * Throws an error if any of the criteria are violated.
     */
    void registerDonors(const std::vector<ShardId>& donorShardIds);

    /**
     * Updates the exponential moving average of the time it takes to fetch an oplog entry from the
     * given donor. Throws an error if the donor has not been registered.
     */
    void updateAverageTimeToFetchOplogEntries(const ShardId& donorShardId,
                                              Milliseconds timeToFetch);

    /**
     * Updates the exponential moving average of the time it takes to apply an oplog entry after it
     * has been fetched from the given donor. Throws an error if the donor has not been registered.
     */
    void updateAverageTimeToApplyOplogEntries(const ShardId& donorShardId,
                                              Milliseconds timeToApply);

    /**
     * Returns the exponential moving average of the time it takes to fetch an oplog entry from the
     * given donor. Throws an error if the donor has not been registered.
     */
    boost::optional<Milliseconds> getAverageTimeToFetchOplogEntries(
        const ShardId& donorShardId) const;

    /**
     * Returns the exponential moving average of the time it takes to apply an oplog entry after it
     * has been fetched from the given donor. Throws an error if the donor has not been registered.
     */
    boost::optional<Milliseconds> getAverageTimeToApplyOplogEntries(
        const ShardId& donorShardId) const;

    /**
     * Returns the maximum exponential moving average of the time it takes to fetch and apply an
     * oplog entry across all donors. Returns none if the metrics for any of the donors are not
     * available yet. If the calculation logging is enabled, logs the average time to fetch and
     * apply oplog entries for each donor.
     */
    boost::optional<Milliseconds> getMaxAverageTimeToFetchAndApplyOplogEntries(
        CalculationLogOption logOption) const;

    void onBatchRetrievedDuringOplogFetching(Milliseconds elapsed);
    void onLocalInsertDuringOplogFetching(const Milliseconds& elapsed);
    void onBatchRetrievedDuringOplogApplying(const Milliseconds& elapsed);
    void onOplogLocalBatchApplied(Milliseconds elapsed);

    boost::optional<ReshardingMetricsTimeInterval> getIntervalFor(TimedPhase phase) const;
    boost::optional<Date_t> getStartFor(TimedPhase phase) const;
    boost::optional<Date_t> getEndFor(TimedPhase phase) const;
    void setStartFor(TimedPhase phase, Date_t date);
    void setEndFor(TimedPhase phase, Date_t date);

    void setStartFor(CoordinatorStateEnum phase, Date_t date);
    void setEndFor(CoordinatorStateEnum phase, Date_t date);

    template <typename TimeUnit>
    boost::optional<TimeUnit> getElapsed(TimedPhase phase, ClockSource* clock) const {
        return _phaseDurations.getElapsed<TimeUnit>(phase, clock);
    }

    template <typename StateOrStateVariant>
    static bool mustRestoreExternallyTrackedRecipientFields(StateOrStateVariant stateOrVariant) {
        if constexpr (std::is_same_v<StateOrStateVariant, State>) {
            return visit([](auto v) { return mustRestoreExternallyTrackedRecipientFieldsImpl(v); },
                         stateOrVariant);
        } else {
            return mustRestoreExternallyTrackedRecipientFieldsImpl(stateOrVariant);
        }
    }

    void deregisterMetrics() {
        _scopedObserver.reset();
    }

    void restoreExternallyTrackedRecipientFields(const ExternallyTrackedRecipientFields& values);

    void reportPhaseDurations(BSONObjBuilder* builder);

    void updateDonorCtx(DonorShardContext& donorCtx);
    void updateRecipientCtx(RecipientShardContext& recipientCtx);

    void onStarted();
    void onSuccess();
    void onFailure();
    void onCanceled();

    void setIsSameKeyResharding(bool isSameKeyResharding);
    void setIndexesToBuild(int64_t numIndexes);
    void setIndexesBuilt(int64_t numIndexes);

private:
    static constexpr auto kNoDate = Date_t::min();
    using UniqueScopedObserver = ReshardingCumulativeMetrics::UniqueScopedObserver;

    template <typename T>
    T getElapsed(const AtomicWord<Date_t>& startTime,
                 const AtomicWord<Date_t>& endTime,
                 ClockSource* clock) const {
        auto start = startTime.load();
        if (start == kNoDate) {
            return T{0};
        }
        auto end = endTime.load();
        if (end == kNoDate) {
            end = clock->now();
        }
        return duration_cast<T>(end - start);
    }

    template <typename T>
    void setState(T state) {
        static_assert(std::is_assignable_v<ReshardingCumulativeMetrics::AnyState, T>);
        _state.store(state);
    }

    void restoreDocumentsProcessed(int64_t documentCount, int64_t totalDocumentsSizeBytes);
    void restoreWritesToStashCollections(int64_t writesToStashCollections);

    ReshardingCumulativeMetrics* getCumulativeMetrics();
    ClockSource* getClockSource() const;
    UniqueScopedObserver registerInstanceMetrics();

    int64_t getInsertsApplied() const;
    int64_t getUpdatesApplied() const;
    int64_t getDeletesApplied() const;
    int64_t getOplogEntriesFetched() const;
    int64_t getOplogEntriesApplied() const;
    void restoreInsertsApplied(int64_t count);
    void restoreUpdatesApplied(int64_t count);
    void restoreDeletesApplied(int64_t count);
    void restoreOplogEntriesFetched(int64_t count);
    void restoreOplogEntriesApplied(int64_t count);

    template <typename TimeUnit>
    void reportDurationsForAllPhases(const TimedPhaseNameMap& names,
                                     ClockSource* clock,
                                     BSONObjBuilder* bob,
                                     boost::optional<TimeUnit> defaultValue = boost::none) const {
        _phaseDurations.reportDurationsForAllPhases(names, clock, bob, defaultValue);
    }

    boost::optional<Milliseconds> getRecipientHighEstimateRemainingTimeMillis(
        CalculationLogOption logOption) const;
    StringData getStateString() const;

    std::string createOperationDescription() const;
    void restoreRecipientSpecificFields(const ReshardingRecipientDocument& document);
    void restoreCoordinatorSpecificFields(const ReshardingCoordinatorDocument& document);
    void restoreIndexBuildDurationFields(const ReshardingRecipientMetrics& metrics);
    ReshardingCumulativeMetrics* getReshardingCumulativeMetrics();

    template <typename T>
    void restoreRoleSpecificFields(const T& document) {
        if constexpr (std::is_same_v<T, ReshardingRecipientDocument>) {
            restoreRecipientSpecificFields(document);
            return;
        }
        if constexpr (std::is_same_v<T, ReshardingCoordinatorDocument>) {
            restoreCoordinatorSpecificFields(document);
            return;
        }
    }

    template <typename T>
    static bool mustRestoreExternallyTrackedRecipientFieldsImpl(T state) {
        static_assert(resharding_metrics::isState<T>);
        if constexpr (std::is_same_v<T, RecipientStateEnum>) {
            return state > RecipientStateEnum::kAwaitingFetchTimestamp;
        } else {
            return false;
        }
    }

    template <typename T>
    void restorePhaseDurationFields(const T& document) {
        static_assert(resharding_metrics::isStateDocument<T>);
        auto metrics = document.getMetrics();
        if (!metrics) {
            return;
        }
        auto copyDurations = metrics->getDocumentCopy();
        if (copyDurations) {
            auto copyingBegin = copyDurations->getStart();
            if (copyingBegin) {
                setStartFor(TimedPhase::kCloning, *copyingBegin);
            }
            auto copyingEnd = copyDurations->getStop();
            if (copyingEnd) {
                setEndFor(TimedPhase::kCloning, *copyingEnd);
            }
        }
        auto applyDurations = metrics->getOplogApplication();
        if (applyDurations) {
            auto applyingBegin = applyDurations->getStart();
            if (applyingBegin) {
                setStartFor(TimedPhase::kApplying, *applyingBegin);
            }
            auto applyingEnd = applyDurations->getStop();
            if (applyingEnd) {
                setEndFor(TimedPhase::kApplying, *applyingEnd);
            }
        }
    }

    template <typename MemberFn, typename... T>
    void invokeIfAllSet(MemberFn&& fn, const boost::optional<T>&... args) {
        if (!(args && ...)) {
            return;
        }
        std::invoke(fn, this, *args...);
    }

    const UUID _instanceId;
    const BSONObj _originalCommand;
    const NamespaceString _sourceNs;
    const Role _role;

    const Date_t _startTime;

    ClockSource* _clockSource;
    ObserverPtr _observer;
    ReshardingCumulativeMetrics* _cumulativeMetrics;

    AtomicWord<int64_t> _approxDocumentsToProcess;
    AtomicWord<int64_t> _documentsProcessed;
    AtomicWord<int64_t> _approxBytesToScan;
    AtomicWord<int64_t> _bytesWritten;

    AtomicWord<int64_t> _writesToStashCollections;

    AtomicWord<Milliseconds> _coordinatorHighEstimateRemainingTimeMillis;
    AtomicWord<Milliseconds> _coordinatorLowEstimateRemainingTimeMillis;

    AtomicWord<int64_t> _readsDuringCriticalSection;
    AtomicWord<int64_t> _writesDuringCriticalSection;

    AtomicWord<ReshardingCumulativeMetrics::AnyState> _state;

    AtomicWord<int64_t> _insertsApplied{0};
    AtomicWord<int64_t> _updatesApplied{0};
    AtomicWord<int64_t> _deletesApplied{0};
    AtomicWord<int64_t> _oplogEntriesApplied{0};
    AtomicWord<int64_t> _oplogEntriesFetched{0};

    // To be used by recipients only. This map stores the OplogLatencyMetrics for each donor that a
    // recipient is copying data from. The map is populated by 'registerDonors' before the oplog
    // fetchers and appliers start running. After that, no inserting or erasing is permitted since
    // the oplog fetchers and appliers only take a shared mutex on the map. The rationale for this
    // setup is to avoid unnecessary lock contention by enabling the fetchers and appliers for
    // different donors to update the metrics without needing to take an exclusive mutex on the
    // map, in addition to an exclusive mutex on their respective OplogLatencyMetrics.
    mutable std::shared_mutex _oplogLatencyMetricsMutex;  // NOLINT
    std::map<ShardId, std::unique_ptr<OplogLatencyMetrics>> _oplogLatencyMetrics;

    resharding_metrics::PhaseDurationTracker _phaseDurations;

    AtomicWord<bool> _ableToEstimateRemainingRecipientTime;

    AtomicWord<bool> _isSameKeyResharding;
    AtomicWord<int64_t> _indexesToBuild;
    AtomicWord<int64_t> _indexesBuilt;

    UniqueScopedObserver _scopedObserver;
    const ReshardingProvenanceEnum _provenance;
};

}  // namespace mongo

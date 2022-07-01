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

#include "mongo/db/namespace_string.h"
#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_observer_interface.h"
#include "mongo/util/duration.h"

namespace mongo {

class ShardingDataTransformInstanceMetrics {
public:
    using Role = ShardingDataTransformMetrics::Role;
    using ObserverPtr = std::unique_ptr<ShardingDataTransformMetricsObserverInterface>;

    ShardingDataTransformInstanceMetrics(UUID instanceId,
                                         BSONObj originalCommand,
                                         NamespaceString sourceNs,
                                         Role role,
                                         Date_t startTime,
                                         ClockSource* clockSource,
                                         ShardingDataTransformCumulativeMetrics* cumulativeMetrics);

    ShardingDataTransformInstanceMetrics(UUID instanceId,
                                         BSONObj originalCommand,
                                         NamespaceString sourceNs,
                                         Role role,
                                         Date_t startTime,
                                         ClockSource* clockSource,
                                         ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
                                         ObserverPtr observer);
    virtual ~ShardingDataTransformInstanceMetrics();

    virtual BSONObj reportForCurrentOp() const noexcept;

    Milliseconds getHighEstimateRemainingTimeMillis() const;
    Milliseconds getLowEstimateRemainingTimeMillis() const;
    virtual Milliseconds getRecipientHighEstimateRemainingTimeMillis() const = 0;
    Date_t getStartTimestamp() const;
    const UUID& getInstanceId() const;

    void onStarted();
    void onSuccess();
    void onFailure();
    void onCanceled();

    void onCopyingBegin();
    void onCopyingEnd();
    void onApplyingBegin();
    void onApplyingEnd();
    void onDocumentsCopied(int64_t documentCount,
                           int64_t totalDocumentsSizeBytes,
                           Milliseconds elapsed);
    Date_t getCopyingBegin() const;
    Date_t getCopyingEnd() const;
    int64_t getDocumentsCopiedCount() const;
    int64_t getBytesCopiedCount() const;
    int64_t getApproxBytesToCopyCount() const;
    void restoreDocumentsCopied(int64_t documentCount, int64_t totalDocumentsSizeBytes);
    void setDocumentsToCopyCounts(int64_t documentCount, int64_t totalDocumentsSizeBytes);
    void setCoordinatorHighEstimateRemainingTimeMillis(Milliseconds milliseconds);
    void setCoordinatorLowEstimateRemainingTimeMillis(Milliseconds milliseconds);
    void onLocalInsertDuringOplogFetching(Milliseconds elapsed);
    void onBatchRetrievedDuringOplogApplying(Milliseconds elapsed);
    void onCloningTotalRemoteBatchRetrieval(Milliseconds elapsed);
    void onOplogLocalBatchApplied(Milliseconds elapsed);
    void onWriteToStashedCollections();

    void onReadDuringCriticalSection();
    void onWriteDuringCriticalSection();
    void onCriticalSectionBegin();
    void onCriticalSectionEnd();

    Role getRole() const;
    Seconds getOperationRunningTimeSecs() const;
    Seconds getCopyingElapsedTimeSecs() const;
    Seconds getApplyingElapsedTimeSecs() const;
    Seconds getCriticalSectionElapsedTimeSecs() const;

    void setLastOpEndingChunkImbalance(int64_t imbalanceCount);

protected:
    static constexpr auto kNoDate = Date_t::min();

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
    void restoreCopyingBegin(Date_t date);
    void restoreCopyingEnd(Date_t date);
    virtual std::string createOperationDescription() const noexcept;
    virtual StringData getStateString() const noexcept;
    void accumulateWritesToStashCollections(int64_t writesToStashCollections);

    ShardingDataTransformCumulativeMetrics* getCumulativeMetrics();
    ClockSource* getClockSource() const;

    const UUID _instanceId;
    const BSONObj _originalCommand;
    const NamespaceString _sourceNs;
    const Role _role;
    static constexpr auto kType = "type";
    static constexpr auto kDescription = "desc";
    static constexpr auto kNamespace = "ns";
    static constexpr auto kOp = "op";
    static constexpr auto kOriginatingCommand = "originatingCommand";
    static constexpr auto kOpTimeElapsed = "totalOperationTimeElapsedSecs";
    static constexpr auto kCriticalSectionTimeElapsed = "totalCriticalSectionTimeElapsedSecs";
    static constexpr auto kRemainingOpTimeEstimated = "remainingOperationTimeEstimatedSecs";
    static constexpr auto kCopyTimeElapsed = "totalCopyTimeElapsedSecs";
    static constexpr auto kApproxDocumentsToCopy = "approxDocumentsToCopy";
    static constexpr auto kApproxBytesToCopy = "approxBytesToCopy";
    static constexpr auto kBytesCopied = "bytesCopied";
    static constexpr auto kCountWritesToStashCollections = "countWritesToStashCollections";
    static constexpr auto kDocumentsCopied = "documentsCopied";
    static constexpr auto kCountWritesDuringCriticalSection = "countWritesDuringCriticalSection";
    static constexpr auto kCountReadsDuringCriticalSection = "countReadsDuringCriticalSection";
    static constexpr auto kCoordinatorState = "coordinatorState";
    static constexpr auto kDonorState = "donorState";
    static constexpr auto kRecipientState = "recipientState";
    static constexpr auto kAllShardsLowestRemainingOperationTimeEstimatedSecs =
        "allShardsLowestRemainingOperationTimeEstimatedSecs";
    static constexpr auto kAllShardsHighestRemainingOperationTimeEstimatedSecs =
        "allShardsHighestRemainingOperationTimeEstimatedSecs";

private:
    const Date_t _startTime;

    ClockSource* _clockSource;
    ObserverPtr _observer;
    ShardingDataTransformCumulativeMetrics* _cumulativeMetrics;
    ShardingDataTransformCumulativeMetrics::DeregistrationFunction _deregister;

    AtomicWord<Date_t> _copyingStartTime;
    AtomicWord<Date_t> _copyingEndTime;
    AtomicWord<int32_t> _approxDocumentsToCopy;
    AtomicWord<int32_t> _documentsCopied;
    AtomicWord<int32_t> _approxBytesToCopy;
    AtomicWord<int32_t> _bytesCopied;

    AtomicWord<int64_t> _writesToStashCollections;

    AtomicWord<Milliseconds> _coordinatorHighEstimateRemainingTimeMillis;
    AtomicWord<Milliseconds> _coordinatorLowEstimateRemainingTimeMillis;

    AtomicWord<Date_t> _criticalSectionStartTime;
    AtomicWord<Date_t> _criticalSectionEndTime;
    AtomicWord<int64_t> _readsDuringCriticalSection;
    AtomicWord<int64_t> _writesDuringCriticalSection;
};

}  // namespace mongo

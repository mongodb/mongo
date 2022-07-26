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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/resharding/resharding_metrics_field_name_provider.h"
#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/sharding_data_transform_instance_metrics.h"
#include "mongo/util/uuid.h"

namespace mongo {

class ReshardingMetrics : public ShardingDataTransformInstanceMetrics {
public:
    using State = stdx::variant<CoordinatorStateEnum, RecipientStateEnum, DonorStateEnum>;
    class DonorState {
    public:
        using MetricsType = ShardingDataTransformCumulativeMetrics::DonorStateEnum;

        explicit DonorState(DonorStateEnum enumVal);
        MetricsType toMetrics() const;
        DonorStateEnum getState() const;

    private:
        const DonorStateEnum _enumVal;
    };

    class RecipientState {
    public:
        using MetricsType = ShardingDataTransformCumulativeMetrics::RecipientStateEnum;

        explicit RecipientState(RecipientStateEnum enumVal);
        MetricsType toMetrics() const;
        RecipientStateEnum getState() const;

    private:
        RecipientStateEnum _enumVal;
    };

    class CoordinatorState {
    public:
        using MetricsType = ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum;

        explicit CoordinatorState(CoordinatorStateEnum enumVal);
        MetricsType toMetrics() const;
        CoordinatorStateEnum getState() const;

    private:
        CoordinatorStateEnum _enumVal;
    };

    ReshardingMetrics(const CommonReshardingMetadata& metadata,
                      Role role,
                      ClockSource* clockSource,
                      ShardingDataTransformCumulativeMetrics* cumulativeMetrics);
    ReshardingMetrics(UUID instanceId,
                      BSONObj shardKey,
                      NamespaceString nss,
                      Role role,
                      Date_t startTime,
                      ClockSource* clockSource,
                      ShardingDataTransformCumulativeMetrics* cumulativeMetrics);
    ~ReshardingMetrics();

    static std::unique_ptr<ReshardingMetrics> makeInstance(UUID instanceId,
                                                           BSONObj shardKey,
                                                           NamespaceString nss,
                                                           Role role,
                                                           Date_t startTime,
                                                           ServiceContext* serviceContext);

    template <typename T>
    static auto initializeFrom(const T& document,
                               ClockSource* clockSource,
                               ShardingDataTransformCumulativeMetrics* cumulativeMetrics) {
        static_assert(resharding_metrics::isStateDocument<T>);
        auto result =
            std::make_unique<ReshardingMetrics>(document.getCommonReshardingMetadata(),
                                                resharding_metrics::getRoleForStateDocument<T>(),
                                                clockSource,
                                                cumulativeMetrics);
        result->setState(resharding_metrics::getState(document));
        result->restoreRoleSpecificFields(document);
        return result;
    }

    template <typename T>
    static auto initializeFrom(const T& document, ServiceContext* serviceContext) {
        return initializeFrom(
            document,
            serviceContext->getFastClockSource(),
            ShardingDataTransformCumulativeMetrics::getForResharding(serviceContext));
    }

    template <typename T>
    void onStateTransition(T before, boost::none_t after) {
        getCumulativeMetrics()->onStateTransition<typename T::MetricsType>(before.toMetrics(),
                                                                           after);
    }

    template <typename T>
    void onStateTransition(boost::none_t before, T after) {
        setState(after.getState());
        getCumulativeMetrics()->onStateTransition<typename T::MetricsType>(before,
                                                                           after.toMetrics());
    }

    template <typename T>
    void onStateTransition(T before, T after) {
        setState(after.getState());
        getCumulativeMetrics()->onStateTransition<typename T::MetricsType>(before.toMetrics(),
                                                                           after.toMetrics());
    }

    void accumulateFrom(const ReshardingOplogApplierProgress& progressDoc);
    BSONObj reportForCurrentOp() const noexcept override;

    void onUpdateApplied();
    void onInsertApplied();
    void onDeleteApplied();
    void onOplogEntriesFetched(int64_t numEntries, Milliseconds elapsed);
    void restoreOplogEntriesFetched(int64_t numEntries);
    void onOplogEntriesApplied(int64_t numEntries);
    void restoreOplogEntriesApplied(int64_t numEntries);
    void onApplyingBegin();
    void onApplyingEnd();

    Seconds getApplyingElapsedTimeSecs() const;
    Date_t getApplyingBegin() const;
    Date_t getApplyingEnd() const;
    Milliseconds getRecipientHighEstimateRemainingTimeMillis() const;

protected:
    virtual StringData getStateString() const noexcept override;
    void restoreApplyingBegin(Date_t date);
    void restoreApplyingEnd(Date_t date);

private:
    std::string createOperationDescription() const noexcept override;
    void restoreRecipientSpecificFields(const ReshardingRecipientDocument& document);
    void restoreCoordinatorSpecificFields(const ReshardingCoordinatorDocument& document);

    template <typename T>
    void setState(T state) {
        static_assert(std::is_assignable_v<State, T>);
        _state.store(state);
    }

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
                restoreCopyingBegin(*copyingBegin);
            }
            auto copyingEnd = copyDurations->getStop();
            if (copyingEnd) {
                restoreCopyingEnd(*copyingEnd);
            }
        }
        auto applyDurations = metrics->getOplogApplication();
        if (applyDurations) {
            auto applyingBegin = applyDurations->getStart();
            if (applyingBegin) {
                restoreApplyingBegin(*applyingBegin);
            }
            auto applyingEnd = applyDurations->getStop();
            if (applyingEnd) {
                restoreApplyingEnd(*applyingEnd);
            }
        }
    }

    AtomicWord<State> _state;
    AtomicWord<int64_t> _deletesApplied;
    AtomicWord<int64_t> _insertsApplied;
    AtomicWord<int64_t> _updatesApplied;
    AtomicWord<int64_t> _oplogEntriesApplied;
    AtomicWord<int64_t> _oplogEntriesFetched;
    AtomicWord<Date_t> _applyingStartTime;
    AtomicWord<Date_t> _applyingEndTime;

    ReshardingMetricsFieldNameProvider* _reshardingFieldNames;

    ShardingDataTransformInstanceMetrics::UniqueScopedObserver _scopedObserver;
};

}  // namespace mongo

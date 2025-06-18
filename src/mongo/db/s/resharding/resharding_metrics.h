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
#include "mongo/db/s/metrics/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/metrics/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_metrics_field_name_provider.h"
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
#include <string>
#include <variant>

#include <boost/optional/optional.hpp>

namespace mongo {

class ReshardingMetrics : public ShardingDataTransformInstanceMetrics {
public:
    using State = ReshardingCumulativeMetrics::AnyState;
    using Base = ShardingDataTransformInstanceMetrics;
    using TimedPhase = resharding_metrics::TimedPhase;

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
                      ShardingDataTransformCumulativeMetrics* cumulativeMetrics);

    ReshardingMetrics(const CommonReshardingMetadata& metadata,
                      Role role,
                      ClockSource* clockSource,
                      ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
                      State state);

    ReshardingMetrics(UUID instanceId,
                      BSONObj shardKey,
                      NamespaceString nss,
                      Role role,
                      Date_t startTime,
                      ClockSource* clockSource,
                      ShardingDataTransformCumulativeMetrics* cumulativeMetrics);

    ReshardingMetrics(
        UUID instanceId,
        BSONObj shardKey,
        NamespaceString nss,
        Role role,
        Date_t startTime,
        ClockSource* clockSource,
        ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
        State state,
        ReshardingProvenanceEnum provenance = ReshardingProvenanceEnum::kReshardCollection);

    ~ReshardingMetrics() override;

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
                    return ShardingDataTransformCumulativeMetrics::getForMoveCollection(
                        serviceContext);
                case ReshardingProvenanceEnum::kBalancerMoveCollection:
                    return ShardingDataTransformCumulativeMetrics::getForBalancerMoveCollection(
                        serviceContext);
                case ReshardingProvenanceEnum::kUnshardCollection:
                    return ShardingDataTransformCumulativeMetrics::getForUnshardCollection(
                        serviceContext);
                case ReshardingProvenanceEnum::kReshardCollection:
                    return ShardingDataTransformCumulativeMetrics::getForResharding(serviceContext);
            }
            MONGO_UNREACHABLE;
        }();

        return initializeFrom(document, serviceContext->getFastClockSource(), cumulativeMetrics);
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

    BSONObj reportForCurrentOp() const override;

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

protected:
    boost::optional<Milliseconds> getRecipientHighEstimateRemainingTimeMillis(
        CalculationLogOption logOption) const override;
    StringData getStateString() const override;

private:
    std::string createOperationDescription() const override;
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

    AtomicWord<bool> _ableToEstimateRemainingRecipientTime;

    AtomicWord<bool> _isSameKeyResharding;
    AtomicWord<int64_t> _indexesToBuild;
    AtomicWord<int64_t> _indexesBuilt;

    ShardingDataTransformInstanceMetrics::UniqueScopedObserver _scopedObserver;
    ReshardingMetricsFieldNameProvider* _reshardingFieldNames;
    const ReshardingProvenanceEnum _provenance;
};

}  // namespace mongo

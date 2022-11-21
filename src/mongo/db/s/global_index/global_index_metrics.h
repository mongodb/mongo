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
#include "mongo/db/s/global_index/global_index_cloner_gen.h"
#include "mongo/db/s/global_index/global_index_cumulative_metrics.h"
#include "mongo/db/s/global_index/global_index_metrics_field_name_provider.h"
#include "mongo/db/s/metrics_state_holder.h"
#include "mongo/db/s/sharding_data_transform_instance_metrics.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace global_index {

// TODO: Remove when actual coordinator doc is implemented.
class GlobalIndexCoordinatorDocument {
public:
    GlobalIndexCoordinatorDocument(CommonGlobalIndexMetadata commonGlobalIndexMetadata)
        : _commonGlobalIndexMetadata(commonGlobalIndexMetadata) {}
    CommonGlobalIndexMetadata& getCommonGlobalIndexMetadata() {
        return _commonGlobalIndexMetadata;
    }

private:
    CommonGlobalIndexMetadata _commonGlobalIndexMetadata;
};

template <class T>
inline constexpr bool isStateDocument =
    std::disjunction_v<std::is_same<T, GlobalIndexClonerDoc>,
                       std::is_same<T, GlobalIndexCoordinatorDocument>>;

class GlobalIndexMetrics : public ShardingDataTransformInstanceMetrics {
public:
    template <typename T>
    inline static ShardingDataTransformMetrics::Role getRoleForStateDocument() {
        static_assert(isStateDocument<T>);
        using Role = ShardingDataTransformMetrics::Role;
        if constexpr (std::is_same_v<T, GlobalIndexCoordinatorDocument>) {
            return Role::kCoordinator;
        } else if constexpr (std::is_same_v<T, GlobalIndexClonerDoc>) {
            return Role::kRecipient;
        }
        MONGO_UNREACHABLE;
    }

    // TODO: Replace with actual Global Index state enums by role
    enum class GlobalIndexCoordinatorStateEnumPlaceholder : int32_t {
        kUnused = -1,
        kInitializing,
        kPreparingToDonate,
        kCloning,
        kApplying,
        kBlockingWrites,
        kAborting,
        kCommitting,
        kDone
    };

    using State =
        stdx::variant<GlobalIndexCoordinatorStateEnumPlaceholder, GlobalIndexClonerStateEnum>;

    class RecipientState {
    public:
        using MetricsType = GlobalIndexCumulativeMetrics::RecipientStateEnum;

        explicit RecipientState(GlobalIndexClonerStateEnum enumVal);
        MetricsType toMetrics() const;
        GlobalIndexClonerStateEnum getState() const;

    private:
        GlobalIndexClonerStateEnum _enumVal;
    };

    class CoordinatorState {
    public:
        using MetricsType = GlobalIndexCumulativeMetrics::CoordinatorStateEnum;

        explicit CoordinatorState(GlobalIndexCoordinatorStateEnumPlaceholder enumVal);
        MetricsType toMetrics() const;
        GlobalIndexCoordinatorStateEnumPlaceholder getState() const;

    private:
        GlobalIndexCoordinatorStateEnumPlaceholder _enumVal;
    };

    GlobalIndexMetrics(UUID instanceId,
                       BSONObj originatingCommand,
                       NamespaceString nss,
                       Role role,
                       Date_t startTime,
                       ClockSource* clockSource,
                       ShardingDataTransformCumulativeMetrics* cumulativeMetrics);
    ~GlobalIndexMetrics();

    static std::unique_ptr<GlobalIndexMetrics> makeInstance(UUID uuid,
                                                            NamespaceString nss,
                                                            Role role,
                                                            BSONObj keyPattern,
                                                            bool unique,
                                                            ServiceContext* serviceContext);

    static BSONObj getOriginalCommand(const CommonGlobalIndexMetadata& metadata);

    template <typename T>
    static auto initializeFrom(const T& document, ServiceContext* serviceContext) {
        static_assert(isStateDocument<T>);
        auto metadata = document.getCommonGlobalIndexMetadata();
        auto result = std::make_unique<GlobalIndexMetrics>(
            metadata.getIndexCollectionUUID(),
            getOriginalCommand(metadata),
            metadata.getNss(),
            getRoleForStateDocument<T>(),
            serviceContext->getFastClockSource()->now(),
            serviceContext->getFastClockSource(),
            ShardingDataTransformCumulativeMetrics::getForGlobalIndexes(serviceContext));
        return result;
    }

    template <typename T>
    void onStateTransition(T before, boost::none_t after) {
        _stateHolder.onStateTransition(before, after);
    }

    template <typename T>
    void onStateTransition(boost::none_t before, T after) {
        _stateHolder.onStateTransition(before, after);
    }

    template <typename T>
    void onStateTransition(T before, T after) {
        _stateHolder.onStateTransition(before, after);
    }

    StringData getStateString() const noexcept override;

protected:
    boost::optional<Milliseconds> getRecipientHighEstimateRemainingTimeMillis() const override;

private:
    GlobalIndexCumulativeMetrics* getGlobalIndexCumulativeMetrics();
    std::string createOperationDescription() const noexcept override;

    MetricsStateHolder<State, GlobalIndexCumulativeMetrics> _stateHolder;
    ShardingDataTransformInstanceMetrics::UniqueScopedObserver _scopedObserver;
    GlobalIndexMetricsFieldNameProvider* _globalIndexFieldNames;
};

}  // namespace global_index
}  // namespace mongo

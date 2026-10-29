// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

class DataToShardsAllocationQueryServiceMock : public DataToShardsAllocationQueryService {
public:
    using Response = std::pair<Timestamp, AllocationToShardsStatus>;

    /**
     * Queue the provided responses in the mock.
     */
    template <typename T>
    requires(std::is_same_v<typename T::value_type, Response>)
    void bufferResponses(const T& responses) {
        std::copy(responses.begin(), responses.end(), std::back_inserter(_responses));
    }

    bool empty() const {
        return _responses.empty();
    }

    void allowAnyClusterTime() {
        _allowAnyClusterTime = true;
    }

    /**
     * Sets a fallback status that will be returned by 'getAllocationToShardsStatus' whenever the
     * buffered response queue is empty (instead of uasserting). Useful for fixtures that need a
     * stable, unbounded response, e.g. always 'kNotAvailable' so that the v2 reader prerequisite
     * check fails and v1 is selected.
     */
    void setDefaultStatus(AllocationToShardsStatus status) {
        _defaultStatus = status;
    }

    /**
     * If the buffered response queue is non-empty, hand out the next buffered status (and check
     * the cluster time when '_allowAnyClusterTime' is false). Otherwise fall back to the default
     * status if one has been set.
     */
    AllocationToShardsStatus getAllocationToShardsStatus(OperationContext* opCtx,
                                                         const Timestamp& clusterTime) override {
        if (empty()) {
            uassert(10612400, "queue should not be empty", _defaultStatus.has_value());
            return *_defaultStatus;
        }
        auto response = _responses.front();
        uassert(10612401,
                str::stream() << "cluster time should be equal to expected cluster time. expected: "
                              << response.first.toStringPretty()
                              << ", actual: " << clusterTime.toStringPretty(),
                _allowAnyClusterTime || clusterTime == response.first);
        _responses.pop_front();
        return response.second;
    }

private:
    std::deque<Response> _responses;

    // If set to true, allows any cluster time for the 'getAllocationToShardsStatus' calls.
    bool _allowAnyClusterTime = false;

    // If set, used as the response when the queue is empty.
    boost::optional<AllocationToShardsStatus> _defaultStatus;
};

/**
 * RAII scopeguard for creating a 'DataToShardsAllocationQueryService' mock instance in the global
 * service context. On construction, the previously-installed decoration (if any) is saved aside;
 * on destruction, that previous decoration is restored. This makes the guard safely nestable
 * fixtures can register a baseline mock and individual tests can stack their own on top.
 */
struct ScopedDataToShardsAllocationQueryServiceMock {
    ScopedDataToShardsAllocationQueryServiceMock(
        const ScopedDataToShardsAllocationQueryServiceMock&) = delete;
    ScopedDataToShardsAllocationQueryServiceMock& operator=(
        const ScopedDataToShardsAllocationQueryServiceMock&) = delete;

    ScopedDataToShardsAllocationQueryServiceMock(std::nullptr_t) {
        _previous =
            DataToShardsAllocationQueryService::swap_forTest(getGlobalServiceContext(), nullptr);
    }

    /**
     * Registers a 'DataToShardsAllocationQueryService' mock instance for a single call that will be
     * valid for the specified cluster time. The mocked call will return the provided status.
     */
    ScopedDataToShardsAllocationQueryServiceMock(Timestamp clusterTime,
                                                 AllocationToShardsStatus status) {
        std::vector<DataToShardsAllocationQueryServiceMock::Response> responses;
        responses.emplace_back(clusterTime, status);

        auto queryService = std::make_unique<DataToShardsAllocationQueryServiceMock>();
        queryService->bufferResponses(std::move(responses));

        _previous = DataToShardsAllocationQueryService::swap_forTest(getGlobalServiceContext(),
                                                                     std::move(queryService));
    }

    /**
     * Registers a 'DataToShardsAllocationQueryService' mock instance for a single call that will be
     * valid for an arbitrary cluster time. The mocked call will return the 'kContinue' status.
     */
    ScopedDataToShardsAllocationQueryServiceMock()
        : ScopedDataToShardsAllocationQueryServiceMock(Timestamp(42, 0),
                                                       AllocationToShardsStatus::kOk) {
        static_cast<DataToShardsAllocationQueryServiceMock*>(
            DataToShardsAllocationQueryService::get(getGlobalServiceContext()))
            ->allowAnyClusterTime();
    }

    /**
     * Registers a 'DataToShardsAllocationQueryService' mock instance whose 'getAllocationToShards-
     * Status' returns 'status' for any cluster time, indefinitely.
     */
    explicit ScopedDataToShardsAllocationQueryServiceMock(AllocationToShardsStatus status) {
        auto queryService = std::make_unique<DataToShardsAllocationQueryServiceMock>();
        queryService->setDefaultStatus(status);
        _previous = DataToShardsAllocationQueryService::swap_forTest(getGlobalServiceContext(),
                                                                     std::move(queryService));
    }

    ~ScopedDataToShardsAllocationQueryServiceMock() {
        DataToShardsAllocationQueryService::swap_forTest(getGlobalServiceContext(),
                                                         std::move(_previous));
    }

private:
    std::unique_ptr<DataToShardsAllocationQueryService> _previous;
};

}  // namespace mongo

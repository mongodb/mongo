/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
     * Check that the provided cluster time is equal to next buffered response's cluster time, and
     * then hand out the buffered status for it.
     */
    AllocationToShardsStatus getAllocationToShardsStatus(OperationContext* opCtx,
                                                         const Timestamp& clusterTime) override {
        uassert(10612400, "queue should not be empty", !empty());
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
};

/**
 * RAII scopeguard for creating a 'DataToShardsAllocationQueryService' mock instance in the global
 * service context. This will create a new 'DataToShardsAllocationQueryServiceMock' instance and
 * register it in the global service context upon creation. Upon destruction, it will unregister the
 * instance from the global service context, so that there will be no more instance registered.
 */
struct ScopedDataToShardsAllocationQueryServiceMock {
    ScopedDataToShardsAllocationQueryServiceMock(
        const ScopedDataToShardsAllocationQueryServiceMock&) = delete;
    ScopedDataToShardsAllocationQueryServiceMock& operator=(
        const ScopedDataToShardsAllocationQueryServiceMock&) = delete;

    ScopedDataToShardsAllocationQueryServiceMock(std::nullptr_t) {
        DataToShardsAllocationQueryServiceMock::set(getGlobalServiceContext(), nullptr);
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

        DataToShardsAllocationQueryService::set(getGlobalServiceContext(), std::move(queryService));
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

    ~ScopedDataToShardsAllocationQueryServiceMock() {
        DataToShardsAllocationQueryService::set(getGlobalServiceContext(), nullptr);
    }
};

}  // namespace mongo

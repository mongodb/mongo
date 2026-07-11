// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * Expresses the status/confidence of collection/database(s) allocation to data shards at a given
 * cluster time.
 */
enum class [[MONGO_MOD_OPEN]] AllocationToShardsStatus {
    // Allocation to shards is not available for the given cluster time, because the system has not
    // been tracking the allocations at that time, or because the information may not be accurate.
    kNotAvailable,

    // The given cluster time is in the future.
    kFutureClusterTime,

    // Allocation to shards is available for the given cluster time, and the given cluster time is
    // in the past.
    kOk,
};

/**
 * Interface for services to determine the status/confidence of collection/database(s) allocation to
 * data shards. Can be used by collection- or database-specific change streams in sharded clusters
 * to precisely target only the shards actually required for the change stream, instead of targeting
 * all shards in the cluster.
 */
class [[MONGO_MOD_OPEN]] DataToShardsAllocationQueryService {
public:
    virtual ~DataToShardsAllocationQueryService() = default;

    /**
     * Service and operation Context bindings.
     */
    static DataToShardsAllocationQueryService* get(ServiceContext* service);
    static DataToShardsAllocationQueryService* get(OperationContext* opCtx);
    static void set(
        ServiceContext* service,
        std::unique_ptr<DataToShardsAllocationQueryService> dataToShardsAllocationQueryService);

    /**
     * Return the status/confidence of collection/database(s) allocation to data shards for the
     * given cluster time.
     */
    virtual AllocationToShardsStatus getAllocationToShardsStatus(OperationContext* opCtx,
                                                                 const Timestamp& clusterTime) = 0;

private:
    /**
     * Swaps current DataToShardsAllocationQueryService with the 'replacement', returning the
     * original service.
     */
    static std::unique_ptr<DataToShardsAllocationQueryService> swap_forTest(
        ServiceContext* service, std::unique_ptr<DataToShardsAllocationQueryService> replacement);

    friend struct ScopedDataToShardsAllocationQueryServiceMock;
};

}  // namespace mongo

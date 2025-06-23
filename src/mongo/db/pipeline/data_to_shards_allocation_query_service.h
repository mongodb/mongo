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

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <memory>

namespace mongo {

/**
 * Expresses the status/confidence of collection/database(s) allocation to data shards at a given
 * cluster time.
 */
enum class AllocationToShardsStatus {
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
class DataToShardsAllocationQueryService {
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
};

}  // namespace mongo

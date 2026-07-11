// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/ns_targeter.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <vector>

// TODO (SERVER-116151): The NSTargeter(s) hierarchy is a legacy implementation. When it is no
// longer needed by BatchWriteExec and bulk_write_exec it should be removed.

namespace mongo {

/**
 * A MockRange represents a range with endpoint that a MockNSTargeter uses to direct writes to
 * a particular endpoint.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] MockRange {
    MockRange(const ShardEndpoint& endpoint, const BSONObj& minKey, const BSONObj& maxKey)
        : endpoint(endpoint), range(minKey, maxKey) {}

    const ShardEndpoint endpoint;
    const ChunkRange range;
};

/**
 * A MockNSTargeter directs writes to particular endpoints based on a list of MockRanges given
 * to the mock targeter on initialization.
 *
 * No refreshing behavior is currently supported.
 */
class [[MONGO_MOD_OPEN]] MockNSTargeter : public NSTargeter {
public:
    MockNSTargeter(const NamespaceString& nss, std::vector<MockRange> mockRanges);

    const NamespaceString& getNS() const override {
        return _nss;
    }

    /**
     * Returns a ShardEndpoint for the doc from the mock ranges. If `chunkRanges` is not nullptr,
     * also populates a set of ChunkRange for the chunks that are targeted.
     */
    ShardEndpoint targetInsert(OperationContext* opCtx, const BSONObj& doc) const override {
        auto endpoints = _targetQuery(doc);
        ASSERT_EQ(1U, endpoints.size());
        return endpoints.front();
    }

    /**
     * Returns the first ShardEndpoint for the query from the mock ranges.  Only can handle
     * queries of the form { field : { $gte : <value>, $lt : <value> } }. If `chunkRanges` is not
     * nullptr, also populates a set of ChunkRange for the chunks that are targeted.
     */
    TargetingResult targetUpdate(OperationContext* opCtx,
                                 const BatchItemRef& itemRef) const override {
        TargetingResult result;

        result.endpoints = _targetQuery(itemRef.getUpdateOp().getFilter());
        return result;
    }

    /**
     * Returns the first ShardEndpoint for the query from the mock ranges.  Only can handle
     * queries of the form { field : { $gte : <value>, $lt : <value> } }. If `chunkRanges` is not
     * nullptr, also populates a set of ChunkRange for the chunks that are targeted.
     */
    TargetingResult targetDelete(OperationContext* opCtx,
                                 const BatchItemRef& itemRef) const override {
        TargetingResult result;

        result.endpoints = _targetQuery(itemRef.getDeleteOp().getFilter());
        return result;
    }

    std::vector<ShardEndpoint> targetAllShards(OperationContext* opCtx) const override {
        std::vector<ShardEndpoint> endpoints;
        for (const auto& range : _mockRanges) {
            endpoints.push_back(range.endpoint);
        }

        return endpoints;
    }

    void noteCouldNotTarget() override {
        // No-op
    }

    void noteStaleCollVersionResponse(OperationContext* opCtx,
                                      const StaleConfigInfo& staleInfo) override {
        // No-op
    }

    void noteStaleDbVersionResponse(OperationContext* opCtx,
                                    const StaleDbRoutingVersion& staleInfo) override {
        // No-op
    }

    bool hasStaleShardResponse() override {
        // No-op
        return false;
    }

    void noteCannotImplicitlyCreateCollectionResponse(
        OperationContext* opCtx, const CannotImplicitlyCreateCollectionInfo& createInfo) override {
        // No-op
    }

    bool refreshIfNeeded(OperationContext* opCtx) override {
        // No-op
        return false;
    }

    bool createCollectionIfNeeded(OperationContext* opCtx) override {
        // No-op
        return false;
    }

    int getAproxNShardsOwningChunks() const override {
        // No-op
        return 0;
    }

    bool isTargetedCollectionSharded() const override {
        // No-op
        return true;
    }

    bool isTrackedTimeSeriesBucketsNamespace() const override {
        return _isTrackedTimeSeriesBucketsNamespace;
    }

    void setIsTrackedTimeSeriesBucketsNamespace(bool isTrackedTimeSeriesBucketsNamespace) {
        _isTrackedTimeSeriesBucketsNamespace = isTrackedTimeSeriesBucketsNamespace;
    }

    bool isTrackedTimeSeriesNamespace() const override {
        return _isTrackedTimeSeriesBucketsNamespace;
    }

    void setIsTrackedTimeSeriesNamespace(bool isTrackedTimeseriesNamespace) {
        _isTrackedTimeSeriesNamespace = isTrackedTimeseriesNamespace;
    }

private:
    /**
     * Returns the first ShardEndpoint for the query from the mock ranges. Only handles queries of
     * the form { field : { $gte : <value>, $lt : <value> } }. If chunkRanges is not nullptr, also
     * populates set of ChunkRange for the chunks that are targeted.
     */
    std::vector<ShardEndpoint> _targetQuery(const BSONObj& query) const;

    NamespaceString _nss;

    std::vector<MockRange> _mockRanges;

    bool _isTrackedTimeSeriesBucketsNamespace = false;

    bool _isTrackedTimeSeriesNamespace = false;
};

[[MONGO_MOD_NEEDS_REPLACEMENT]] void assertEndpointsEqual(const ShardEndpoint& endpointA,
                                                          const ShardEndpoint& endpointB);

}  // namespace mongo

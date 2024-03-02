/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <set>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/ns_targeter.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

/**
 * A MockRange represents a range with endpoint that a MockNSTargeter uses to direct writes to
 * a particular endpoint.
 */
struct MockRange {
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
class MockNSTargeter : public NSTargeter {
public:
    MockNSTargeter(const NamespaceString& nss, std::vector<MockRange> mockRanges);

    const NamespaceString& getNS() const override {
        return _nss;
    }

    /**
     * Returns a ShardEndpoint for the doc from the mock ranges. If `chunkRanges` is not nullptr,
     * also populates a set of ChunkRange for the chunks that are targeted.
     */
    ShardEndpoint targetInsert(OperationContext* opCtx,
                               const BSONObj& doc,
                               std::set<ChunkRange>* chunkRanges = nullptr) const override {
        auto endpoints = _targetQuery(doc, chunkRanges);
        ASSERT_EQ(1U, endpoints.size());
        return endpoints.front();
    }

    /**
     * Returns the first ShardEndpoint for the query from the mock ranges.  Only can handle
     * queries of the form { field : { $gte : <value>, $lt : <value> } }. If `chunkRanges` is not
     * nullptr, also populates a set of ChunkRange for the chunks that are targeted.
     */
    std::vector<ShardEndpoint> targetUpdate(
        OperationContext* opCtx,
        const BatchItemRef& itemRef,
        bool* useTwoPhaseWriteProtocol = nullptr,
        bool* isNonTargetedWriteWithoutShardKeyWithExactId = nullptr,
        std::set<ChunkRange>* chunkRanges = nullptr) const override {
        return _targetQuery(itemRef.getUpdateRef().getFilter(), chunkRanges);
    }

    /**
     * Returns the first ShardEndpoint for the query from the mock ranges.  Only can handle
     * queries of the form { field : { $gte : <value>, $lt : <value> } }. If `chunkRanges` is not
     * nullptr, also populates a set of ChunkRange for the chunks that are targeted.
     */
    std::vector<ShardEndpoint> targetDelete(
        OperationContext* opCtx,
        const BatchItemRef& itemRef,
        bool* useTwoPhaseWriteProtocol = nullptr,
        bool* isNonTargetedWriteWithoutShardKeyWithExactId = nullptr,
        std::set<ChunkRange>* chunkRanges = nullptr) const override {
        return _targetQuery(itemRef.getDeleteRef().getFilter(), chunkRanges);
    }

    std::vector<ShardEndpoint> targetAllShards(
        OperationContext* opCtx, std::set<ChunkRange>* chunkRanges = nullptr) const override {
        std::vector<ShardEndpoint> endpoints;
        for (const auto& range : _mockRanges) {
            endpoints.push_back(range.endpoint);
        }

        return endpoints;
    }

    void noteCouldNotTarget() override {
        // No-op
    }

    void noteStaleShardResponse(OperationContext* opCtx,
                                const ShardEndpoint& endpoint,
                                const StaleConfigInfo& staleInfo) override {
        // No-op
    }

    void noteStaleDbResponse(OperationContext* opCtx,
                             const ShardEndpoint& endpoint,
                             const StaleDbRoutingVersion& staleInfo) override {
        // No-op
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

    int getNShardsOwningChunks() const override {
        // No-op
        return 0;
    }

    bool isTargetedCollectionSharded() const override {
        // No-op
        return false;
    }

    bool isTrackedTimeSeriesBucketsNamespace() const override {
        return _isTrackedTimeSeriesBucketsNamespace;
    }

    void setIsTrackedTimeSeriesBucketsNamespace(bool isTrackedTimeSeriesBucketsNamespace) {
        _isTrackedTimeSeriesBucketsNamespace = isTrackedTimeSeriesBucketsNamespace;
    }

private:
    /**
     * Returns the first ShardEndpoint for the query from the mock ranges. Only handles queries of
     * the form { field : { $gte : <value>, $lt : <value> } }. If chunkRanges is not nullptr, also
     * populates set of ChunkRange for the chunks that are targeted.
     */
    std::vector<ShardEndpoint> _targetQuery(const BSONObj& query,
                                            std::set<ChunkRange>* chunkRanges) const;

    NamespaceString _nss;

    std::vector<MockRange> _mockRanges;

    bool _isTrackedTimeSeriesBucketsNamespace = false;
};

void assertEndpointsEqual(const ShardEndpoint& endpointA, const ShardEndpoint& endpointB);

}  // namespace mongo

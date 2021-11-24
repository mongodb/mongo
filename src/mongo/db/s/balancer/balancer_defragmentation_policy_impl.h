/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/balancer_defragmentation_policy.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/s/catalog/type_collection.h"

namespace mongo {
class BalancerDefragmentationPolicyImpl : public BalancerDefragmentationPolicy {
    BalancerDefragmentationPolicyImpl(const BalancerDefragmentationPolicyImpl&) = delete;
    BalancerDefragmentationPolicyImpl& operator=(const BalancerDefragmentationPolicyImpl&) = delete;

public:
    BalancerDefragmentationPolicyImpl(ClusterStatistics* clusterStats)
        : _clusterStats(clusterStats) {}

    ~BalancerDefragmentationPolicyImpl() {}

    bool isDefragmentingCollection(const UUID& uuid) override {
        return _defragmentationStates.contains(uuid);
    }

    SemiFuture<DefragmentationAction> getNextStreamingAction() override;

    void acknowledgeMergeResult(MergeInfo action, const Status& result) override;

    void acknowledgeAutoSplitVectorResult(AutoSplitVectorInfo action,
                                          const StatusWith<std::vector<BSONObj>>& result) override;

    void acknowledgeSplitResult(SplitInfoWithKeyPattern action, const Status& result) override;

    void acknowledgeDataSizeResult(OperationContext* opCtx,
                                   DataSizeInfo action,
                                   const StatusWith<DataSizeResponse>& result) override;

    void closeActionStream() override;

    void beginNewCollection(OperationContext* opCtx, const UUID& uuid) override;

    void removeCollection(OperationContext* opCtx, const UUID& uuid) override;

private:
    static constexpr int kMaxConcurrentOperations = 50;

    // Data structures used to keep track of the defragmentation state.
    struct CollectionDefragmentationState {
        NamespaceString nss;
        DefragmentationPhaseEnum phase;
        int64_t maxChunkSizeBytes;
        BSONObj collectionShardKey;
        std::queue<DefragmentationAction> queuedActions;
        std::vector<ChunkType> chunkList;
        ZoneInfo zones;
    };

    boost::optional<DefragmentationAction> _nextStreamingAction();

    /**
     * Returns next phase 1 merge action for the collection if there is one and boost::none
     * otherwise.
     */
    boost::optional<MergeInfo> _getCollectionMergeAction(
        CollectionDefragmentationState& collectionInfo) {
        return boost::none;
    }

    /**
     * Returns next phase 3 split action for the collection if there is one and boost::none
     * otherwise.
     */
    boost::optional<SplitInfoWithKeyPattern> _getCollectionSplitAction(
        CollectionDefragmentationState& collectionInfo) {
        return boost::none;
    }

    /**
     * Build the shardToChunk map for the namespace. Requires a scan of the config.chunks
     * collection.
     */
    void _initializeCollectionState(OperationContext* opCtx, const UUID& uuid);

    /**
     * Write the new phase to the defragmentationPhase field in config.collections. If phase is not
     * set, the field will be removed.
     */
    void _persistPhaseUpdate(OperationContext* opCtx,
                             boost::optional<DefragmentationPhaseEnum> phase,
                             const UUID& uuid);

    /**
     * Remove all datasize fields from config.chunks for the given namespace.
     */
    void _clearDataSizeInformation(OperationContext* opCtx, const UUID& uuid);

    void _processEndOfAction(WithLock,
                             const UUID& uuid,
                             const boost::optional<DefragmentationAction>& nextActionOnNamespace);

    Mutex _streamingMutex = MONGO_MAKE_LATCH("BalancerChunkMergerImpl::_streamingMutex");
    unsigned _concurrentStreamingOps{0};
    boost::optional<Promise<DefragmentationAction>> _nextStreamingActionPromise{boost::none};
    bool _streamClosed{false};

    ClusterStatistics* const _clusterStats;
    stdx::unordered_map<UUID, CollectionDefragmentationState, UUID::Hash> _defragmentationStates;
};
}  // namespace mongo

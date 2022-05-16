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

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/s/balancer/cluster_chunks_resize_policy.h"
#include "mongo/s/catalog/type_collection.h"

namespace mongo {

struct ActionRequestInfo {
    ActionRequestInfo(ChunkRange&& chunkRange, const ShardId& host, SplitPoints&& splitPoints)
        : chunkRange(std::move(chunkRange)), host(host), splitPoints(std::move(splitPoints)) {}
    ChunkRange chunkRange;
    ShardId host;
    SplitPoints splitPoints;
};

class CollectionState {
public:
    CollectionState(const CollectionType& coll,
                    std::vector<ActionRequestInfo>&& initialRequests,
                    int defaultMaxChunksSizeBytes);

    CollectionState(CollectionState&& rhs) = default;

    CollectionState& operator=(CollectionState&& rhs) = default;

    boost::optional<DefragmentationAction> popNextAction(OperationContext* opCtx);

    void actionCompleted(std::vector<ActionRequestInfo>&& followUpRequests);

    void errorDetectedOnActionCompleted(OperationContext* opCtx);

    bool restartNeeded() const;

    bool fullyProcessed() const;

    const NamespaceString& getNss() const;

    const UUID& getUuid() const;

    static ActionRequestInfo composeAutoSplitVectorRequest(const BSONObj& minKey,
                                                           const BSONObj& maxKey,
                                                           const ShardId& shard);

    static ActionRequestInfo composeSplitChunkRequest(const BSONObj& minKey,
                                                      const BSONObj& maxKey,
                                                      const ShardId& shard,
                                                      SplitPoints splitPoints);

private:
    NamespaceString _nss;
    UUID _uuid;
    BSONObj _keyPattern;
    OID _epoch;
    Timestamp _creationTime;
    int64_t _maxChunkSizeBytes;
    std::vector<ActionRequestInfo> _pendingRequests;
    int _numOutstandingActions;
    bool _restartRequested;

    void _requestRestart(OperationContext* opCtx);
};

class ClusterChunksResizePolicyImpl : public ClusterChunksResizePolicy {

public:
    ClusterChunksResizePolicyImpl(const std::function<void()>& onStateUpdated);

    ~ClusterChunksResizePolicyImpl();

    SharedSemiFuture<void> activate(OperationContext* opCtx,
                                    int64_t defaultMaxChunksSizeBytes) override;

    bool isActive() override;

    void stop() override;

    StringData getName() const override;

    boost::optional<DefragmentationAction> getNextStreamingAction(OperationContext* opCtx) override;

    void applyActionResult(OperationContext* opCtx,
                           const DefragmentationAction& action,
                           const DefragmentationActionResponse& result) override;

private:
    const std::string kPolicyName{"ClusterChunksResizePolicy"};

    const size_t kMaxCollectionsBeingProcessed{3};

    const std::function<void()> _onStateUpdated;

    Mutex _stateMutex = MONGO_MAKE_LATCH("ClusterChunksResizePolicyImpl::_stateMutex");

    boost::optional<SharedPromise<void>> _activeRequestPromise{boost::none};
    std::unique_ptr<mongo::DBClientCursor> _unprocessedCollections;
    stdx::unordered_map<UUID, CollectionState, UUID::Hash> _collectionsBeingProcessed;
    int64_t _defaultMaxChunksSizeBytes{-1};

    boost::optional<CollectionState> _buildInitialStateFor(OperationContext* opCtx,
                                                           const NamespaceString& nss);

    boost::optional<CollectionState> _buildInitialStateFor(OperationContext* opCtx,
                                                           const CollectionType& coll);
};
}  // namespace mongo

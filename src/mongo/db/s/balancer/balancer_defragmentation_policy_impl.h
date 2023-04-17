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

/**
 * Interface describing the interactions that the defragmentation policy can establish with the
 * phase of the algorithm that is currently active on a collection.
 * With the exception getType(), its methods do not guarantee thread safety.
 */
class DefragmentationPhase {
public:
    virtual ~DefragmentationPhase() {}

    virtual DefragmentationPhaseEnum getType() const = 0;

    virtual DefragmentationPhaseEnum getNextPhase() const = 0;

    virtual boost::optional<BalancerStreamAction> popNextStreamableAction(
        OperationContext* opCtx) = 0;

    virtual boost::optional<MigrateInfo> popNextMigration(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* availableShards) = 0;

    virtual void applyActionResult(OperationContext* opCtx,
                                   const BalancerStreamAction& action,
                                   const BalancerStreamActionResponse& response) = 0;

    virtual BSONObj reportProgress() const = 0;

    virtual bool isComplete() const = 0;

    virtual void userAbort() = 0;

protected:
    static constexpr uint64_t kSmallChunkSizeThresholdPctg = 25;
};

class BalancerDefragmentationPolicyImpl : public BalancerDefragmentationPolicy {
    BalancerDefragmentationPolicyImpl(const BalancerDefragmentationPolicyImpl&) = delete;
    BalancerDefragmentationPolicyImpl& operator=(const BalancerDefragmentationPolicyImpl&) = delete;

public:
    BalancerDefragmentationPolicyImpl(ClusterStatistics* clusterStats,
                                      const std::function<void()>& onStateUpdated)
        : _clusterStats(clusterStats), _onStateUpdated(onStateUpdated) {}

    ~BalancerDefragmentationPolicyImpl() {}

    void interruptAllDefragmentations() override;

    bool isDefragmentingCollection(const UUID& uuid) override;

    virtual BSONObj reportProgressOn(const UUID& uuid) override;

    MigrateInfoVector selectChunksToMove(OperationContext* opCtx,
                                         stdx::unordered_set<ShardId>* availableShards) override;

    StringData getName() const override;

    boost::optional<BalancerStreamAction> getNextStreamingAction(OperationContext* opCtx) override;

    void applyActionResult(OperationContext* opCtx,
                           const BalancerStreamAction& action,
                           const BalancerStreamActionResponse& response) override;

    void startCollectionDefragmentations(OperationContext* opCtx) override;

    void abortCollectionDefragmentation(OperationContext* opCtx,
                                        const NamespaceString& nss) override;

private:
    /**
     * Advances the defragmentation state of the specified collection to the next actionable phase
     * (or sets the related DefragmentationPhase object to nullptr if nothing more can be done).
     */
    bool _advanceToNextActionablePhase(OperationContext* opCtx, const UUID& collUuid);

    /**
     * Move to the next phase and persist the phase change. This will end defragmentation if the
     * next phase is kFinished.
     * Must be called while holding the _stateMutex.
     */
    std::unique_ptr<DefragmentationPhase> _transitionPhases(OperationContext* opCtx,
                                                            const CollectionType& coll,
                                                            DefragmentationPhaseEnum nextPhase,
                                                            bool shouldPersistPhase = true);

    /**
     * Builds the defragmentation phase object matching the current state of the passed
     * collection and sets it into _defragmentationStates.
     */
    void _initializeCollectionState(WithLock, OperationContext* opCtx, const CollectionType& coll);

    /**
     * Write the new phase to the defragmentationPhase field in config.collections. If phase is
     * kFinished, the field will be removed.
     * Must be called while holding the _stateMutex.
     */
    void _persistPhaseUpdate(OperationContext* opCtx,
                             DefragmentationPhaseEnum phase,
                             const UUID& uuid);

    /**
     * Remove all datasize fields from config.chunks for the given namespace.
     * Must be called while holding the _stateMutex.
     */
    void _clearDefragmentationState(OperationContext* opCtx, const UUID& uuid);

    const std::string kPolicyName{"BalancerDefragmentationPolicy"};

    Mutex _stateMutex = MONGO_MAKE_LATCH("BalancerChunkMergerImpl::_stateMutex");

    ClusterStatistics* const _clusterStats;

    const std::function<void()> _onStateUpdated;

    stdx::unordered_map<UUID, std::unique_ptr<DefragmentationPhase>, UUID::Hash>
        _defragmentationStates;
};
}  // namespace mongo

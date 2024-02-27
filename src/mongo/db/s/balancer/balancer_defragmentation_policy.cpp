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

#include "mongo/db/s/balancer/balancer_defragmentation_policy.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <compare>
#include <cstddef>
#include <fmt/format.h>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


using namespace fmt::literals;

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(skipDefragmentationPhaseTransition);
MONGO_FAIL_POINT_DEFINE(afterBuildingNextDefragmentationPhase);

using ShardStatistics = ClusterStatistics::ShardStatistics;

const std::string kCurrentPhase("currentPhase");
const std::string kProgress("progress");
const std::string kNoPhase("none");
const std::string kRemainingChunksToProcess("remainingChunksToProcess");

static constexpr int64_t kBigChunkMarker = std::numeric_limits<int64_t>::max();

ShardVersion getShardVersion(OperationContext* opCtx,
                             const ShardId& shardId,
                             const NamespaceString& nss) {
    auto cri = Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfo(opCtx, nss);
    return cri.getShardVersion(shardId);
}

std::vector<ChunkType> getCollectionChunks(OperationContext* opCtx, const CollectionType& coll) {
    auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    return uassertStatusOK(
        catalogClient->getChunks(opCtx,
                                 BSON(ChunkType::collectionUUID() << coll.getUuid()) /*query*/,
                                 BSON(ChunkType::min() << 1) /*sort*/,
                                 boost::none /*limit*/,
                                 nullptr /*opTime*/,
                                 coll.getEpoch(),
                                 coll.getTimestamp(),
                                 repl::ReadConcernLevel::kLocalReadConcern,
                                 boost::none));
}

uint64_t getCollectionMaxChunkSizeBytes(OperationContext* opCtx, const CollectionType& coll) {
    const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));
    return coll.getMaxChunkSizeBytes().value_or(balancerConfig->getMaxChunkSizeBytes());
}

ZoneInfo getCollectionZones(OperationContext* opCtx, const CollectionType& coll) {
    auto zones = uassertStatusOK(
        ZoneInfo::getZonesForCollection(opCtx, coll.getNss(), coll.getKeyPattern()));
    return zones;
}

bool isRetriableForDefragmentation(const Status& status) {
    if (ErrorCodes::isA<ErrorCategory::RetriableError>(status))
        return true;

    if (status == ErrorCodes::StaleConfig) {
        if (auto staleInfo = status.extraInfo<StaleConfigInfo>()) {
            // If the staleInfo error contains a "wanted" version, this means the donor shard which
            // returned this error has its versioning information up-to-date (as opposed to UNKNOWN)
            // and it couldn't find the chunk that the defragmenter expected. Such a situation can
            // only arise as a result of manual split/merge/move concurrently with the defragmenter.
            return !staleInfo->getVersionWanted();
        }
    }

    return false;
}

void handleActionResult(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const UUID& uuid,
                        const DefragmentationPhaseEnum currentPhase,
                        const Status& status,
                        std::function<void()> onSuccess,
                        std::function<void()> onRetriableError,
                        std::function<void()> onNonRetriableError) {
    if (status.isOK()) {
        onSuccess();
        return;
    }

    if (status == ErrorCodes::StaleConfig) {
        if (auto staleInfo = status.extraInfo<StaleConfigInfo>()) {
            Grid::get(opCtx)
                ->catalogCache()
                ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                    nss, staleInfo->getVersionWanted(), staleInfo->getShardId());
        }
    }

    if (isRetriableForDefragmentation(status)) {
        LOGV2_DEBUG(6261701,
                    1,
                    "Hit retriable error while defragmenting collection",
                    logAttrs(nss),
                    "uuid"_attr = uuid,
                    "currentPhase"_attr = currentPhase,
                    "error"_attr = redact(status));
        onRetriableError();
    } else {
        LOGV2_ERROR(6258601,
                    "Defragmentation for collection hit non-retriable error",
                    logAttrs(nss),
                    "uuid"_attr = uuid,
                    "currentPhase"_attr = currentPhase,
                    "error"_attr = redact(status));
        onNonRetriableError();
    }
}

bool areMergeable(const ChunkType& firstChunk,
                  const ChunkType& secondChunk,
                  const ZoneInfo& collectionZones) {
    return firstChunk.getShard() == secondChunk.getShard() &&
        collectionZones.getZoneForRange(firstChunk.getRange()) ==
        collectionZones.getZoneForRange(secondChunk.getRange()) &&
        SimpleBSONObjComparator::kInstance.evaluate(firstChunk.getMax() == secondChunk.getMin());
}

class MergeAndMeasureChunksPhase : public DefragmentationPhase {
public:
    static std::unique_ptr<MergeAndMeasureChunksPhase> build(OperationContext* opCtx,
                                                             const CollectionType& coll) {
        auto collectionChunks = getCollectionChunks(opCtx, coll);
        const auto collectionZones = getCollectionZones(opCtx, coll);

        // Calculate small chunk threshold to limit dataSize commands
        const auto maxChunkSizeBytes = getCollectionMaxChunkSizeBytes(opCtx, coll);
        const int64_t smallChunkSizeThreshold =
            (maxChunkSizeBytes / 100) * kSmallChunkSizeThresholdPctg;

        stdx::unordered_map<ShardId, PendingActions> pendingActionsByShards;
        // Find ranges of chunks; for single-chunk ranges, request DataSize; for multi-range, issue
        // merge
        while (!collectionChunks.empty()) {
            auto upperRangeBound = std::prev(collectionChunks.cend());
            auto lowerRangeBound = upperRangeBound;
            while (lowerRangeBound != collectionChunks.cbegin() &&
                   areMergeable(*std::prev(lowerRangeBound), *lowerRangeBound, collectionZones)) {
                --lowerRangeBound;
            }
            if (lowerRangeBound != upperRangeBound) {
                pendingActionsByShards[upperRangeBound->getShard()].rangesToMerge.emplace_back(
                    lowerRangeBound->getMin(), upperRangeBound->getMax());
            } else {
                if (!upperRangeBound->getEstimatedSizeBytes().has_value()) {
                    pendingActionsByShards[upperRangeBound->getShard()]
                        .rangesWithoutDataSize.emplace_back(upperRangeBound->getMin(),
                                                            upperRangeBound->getMax());
                }
            }
            collectionChunks.erase(lowerRangeBound, std::next(upperRangeBound));
        }
        return std::unique_ptr<MergeAndMeasureChunksPhase>(
            new MergeAndMeasureChunksPhase(coll.getNss(),
                                           coll.getUuid(),
                                           coll.getKeyPattern().toBSON(),
                                           smallChunkSizeThreshold,
                                           std::move(pendingActionsByShards)));
    }

    DefragmentationPhaseEnum getType() const override {
        return DefragmentationPhaseEnum::kMergeAndMeasureChunks;
    }

    DefragmentationPhaseEnum getNextPhase() const override {
        return _nextPhase;
    }

    boost::optional<BalancerStreamAction> popNextStreamableAction(
        OperationContext* opCtx) override {
        boost::optional<BalancerStreamAction> nextAction = boost::none;
        if (!_pendingActionsByShards.empty()) {
            auto it = _shardToProcess ? _pendingActionsByShards.find(*_shardToProcess)
                                      : _pendingActionsByShards.begin();

            tassert(8245212,
                    "Shard to process not found in pending actions",
                    it != _pendingActionsByShards.end());

            auto& [shardId, pendingActions] = *it;
            auto shardVersion = getShardVersion(opCtx, shardId, _nss);

            if (pendingActions.rangesWithoutDataSize.size() > pendingActions.rangesToMerge.size()) {
                const auto& rangeToMeasure = pendingActions.rangesWithoutDataSize.back();
                nextAction = boost::optional<BalancerStreamAction>(
                    DataSizeInfo(shardId,
                                 _nss,
                                 _uuid,
                                 rangeToMeasure,
                                 shardVersion,
                                 _shardKey,
                                 true /* estimate */,
                                 _smallChunkSizeThresholdBytes /* maxSize */));
                pendingActions.rangesWithoutDataSize.pop_back();
            } else if (!pendingActions.rangesToMerge.empty()) {
                const auto& rangeToMerge = pendingActions.rangesToMerge.back();
                nextAction = boost::optional<BalancerStreamAction>(
                    MergeInfo(shardId, _nss, _uuid, shardVersion.placementVersion(), rangeToMerge));
                pendingActions.rangesToMerge.pop_back();
            }
            if (nextAction.has_value()) {
                ++_outstandingActions;
                if (pendingActions.rangesToMerge.empty() &&
                    pendingActions.rangesWithoutDataSize.empty()) {
                    it = _pendingActionsByShards.erase(it, std::next(it));
                } else {
                    ++it;
                }
            }
            if (it != _pendingActionsByShards.end()) {
                _shardToProcess = it->first;
            } else {
                _shardToProcess = boost::none;
            }
        }
        return nextAction;
    }

    boost::optional<MigrateInfo> popNextMigration(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* availableShards) override {
        return boost::none;
    }

    void applyActionResult(OperationContext* opCtx,
                           const BalancerStreamAction& action,
                           const BalancerStreamActionResponse& response) override {
        ScopeGuard scopedGuard([&] { --_outstandingActions; });
        if (_aborted) {
            return;
        }
        visit(OverloadedVisitor{
                  [&](const MergeInfo& mergeAction) {
                      auto& mergeResponse = get<Status>(response);
                      auto& shardingPendingActions = _pendingActionsByShards[mergeAction.shardId];
                      handleActionResult(
                          opCtx,
                          _nss,
                          _uuid,
                          getType(),
                          mergeResponse,
                          [&]() {
                              shardingPendingActions.rangesWithoutDataSize.emplace_back(
                                  mergeAction.chunkRange);
                          },
                          [&]() {
                              shardingPendingActions.rangesToMerge.emplace_back(
                                  mergeAction.chunkRange);
                          },
                          [&]() { _abort(getType()); });
                  },
                  [&](const DataSizeInfo& dataSizeAction) {
                      auto& dataSizeResponse = get<StatusWith<DataSizeResponse>>(response);
                      handleActionResult(
                          opCtx,
                          _nss,
                          _uuid,
                          getType(),
                          dataSizeResponse.getStatus(),
                          [&]() {
                              ChunkType chunk(dataSizeAction.uuid,
                                              dataSizeAction.chunkRange,
                                              dataSizeAction.version.placementVersion(),
                                              dataSizeAction.shardId);
                              auto catalogManager = ShardingCatalogManager::get(opCtx);
                              // Max out the chunk size if it has has been estimated as bigger
                              // than _smallChunkSizeThresholdBytes; this will exlude the
                              // chunk from the list of candidates considered by
                              // MoveAndMergeChunksPhase
                              auto estimatedSize = dataSizeResponse.getValue().maxSizeReached
                                  ? kBigChunkMarker
                                  : dataSizeResponse.getValue().sizeBytes;
                              catalogManager->setChunkEstimatedSize(
                                  opCtx,
                                  chunk,
                                  estimatedSize,
                                  ShardingCatalogClient::kMajorityWriteConcern);
                          },
                          [&]() {
                              auto& shardingPendingActions =
                                  _pendingActionsByShards[dataSizeAction.shardId];
                              shardingPendingActions.rangesWithoutDataSize.emplace_back(
                                  dataSizeAction.chunkRange);
                          },
                          [&]() { _abort(getType()); });
                  },
                  [](const MigrateInfo& _) {
                      uasserted(ErrorCodes::BadValue, "Unexpected action type");
                  },
                  [](const MergeAllChunksOnShardInfo& _) {
                      uasserted(ErrorCodes::BadValue, "Unexpected action type");
                  }},
              action);
    }

    bool isComplete() const override {
        return _pendingActionsByShards.empty() && _outstandingActions == 0;
    }

    void userAbort() override {
        _abort(DefragmentationPhaseEnum::kFinished);
    }

    BSONObj reportProgress() const override {

        size_t rangesToMerge = 0, rangesWithoutDataSize = 0;
        for (const auto& [_, pendingActions] : _pendingActionsByShards) {
            rangesToMerge += pendingActions.rangesToMerge.size();
            rangesWithoutDataSize += pendingActions.rangesWithoutDataSize.size();
        }
        auto remainingChunksToProcess = static_cast<long long>(_outstandingActions) +
            static_cast<long long>(rangesToMerge) + static_cast<long long>(rangesWithoutDataSize);

        return BSON(kRemainingChunksToProcess << remainingChunksToProcess);
    }

private:
    struct PendingActions {
        std::vector<ChunkRange> rangesToMerge;
        std::vector<ChunkRange> rangesWithoutDataSize;
    };
    MergeAndMeasureChunksPhase(
        const NamespaceString& nss,
        const UUID& uuid,
        const BSONObj& shardKey,
        const int64_t smallChunkSizeThresholdBytes,
        stdx::unordered_map<ShardId, PendingActions>&& pendingActionsByShards)
        : _nss(nss),
          _uuid(uuid),
          _shardKey(shardKey),
          _smallChunkSizeThresholdBytes(smallChunkSizeThresholdBytes),
          _pendingActionsByShards(std::move(pendingActionsByShards)) {}

    void _abort(const DefragmentationPhaseEnum nextPhase) {
        _aborted = true;
        _nextPhase = nextPhase;
        _pendingActionsByShards.clear();
    }

    const NamespaceString _nss;
    const UUID _uuid;
    const BSONObj _shardKey;
    const int64_t _smallChunkSizeThresholdBytes;
    stdx::unordered_map<ShardId, PendingActions> _pendingActionsByShards;
    boost::optional<ShardId> _shardToProcess;
    size_t _outstandingActions{0};
    bool _aborted{false};
    DefragmentationPhaseEnum _nextPhase{DefragmentationPhaseEnum::kMoveAndMergeChunks};
};

class MoveAndMergeChunksPhase : public DefragmentationPhase {
public:
    static std::unique_ptr<MoveAndMergeChunksPhase> build(
        OperationContext* opCtx,
        const CollectionType& coll,
        std::vector<ShardStatistics>&& collectionShardStats) {
        auto collectionZones = getCollectionZones(opCtx, coll);

        stdx::unordered_map<ShardId, ShardInfo> shardInfos;
        for (const auto& shardStats : collectionShardStats) {
            shardInfos.emplace(shardStats.shardId,
                               ShardInfo(shardStats.currSizeBytes, shardStats.isDraining));
        }

        auto collectionChunks = getCollectionChunks(opCtx, coll);
        const auto maxChunkSizeBytes = getCollectionMaxChunkSizeBytes(opCtx, coll);
        const uint64_t smallChunkSizeThresholdBytes =
            (maxChunkSizeBytes / 100) * kSmallChunkSizeThresholdPctg;

        return std::unique_ptr<MoveAndMergeChunksPhase>(
            new MoveAndMergeChunksPhase(coll.getNss(),
                                        coll.getUuid(),
                                        std::move(collectionChunks),
                                        std::move(shardInfos),
                                        std::move(collectionZones),
                                        smallChunkSizeThresholdBytes,
                                        maxChunkSizeBytes));
    }

    DefragmentationPhaseEnum getType() const override {
        return DefragmentationPhaseEnum::kMoveAndMergeChunks;
    }

    DefragmentationPhaseEnum getNextPhase() const override {
        return _nextPhase;
    }

    boost::optional<BalancerStreamAction> popNextStreamableAction(
        OperationContext* opCtx) override {
        if (_actionableMerges.empty()) {
            return boost::none;
        }

        _outstandingMerges.push_back(std::move(_actionableMerges.front()));
        _actionableMerges.pop_front();
        const auto& nextRequest = _outstandingMerges.back();
        auto version = getShardVersion(opCtx, nextRequest.getDestinationShard(), _nss);
        return boost::optional<BalancerStreamAction>(
            nextRequest.asMergeInfo(_uuid, _nss, version.placementVersion()));
    }

    boost::optional<MigrateInfo> popNextMigration(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* availableShards) override {
        for (const auto& shardId : _shardProcessingOrder) {
            if (availableShards->count(shardId) == 0) {
                // the shard is already busy in a migration
                continue;
            }

            ChunkRangeInfoIterator nextSmallChunk;
            std::list<ChunkRangeInfoIterator> candidateSiblings;
            if (!_findNextSmallChunkInShard(
                    shardId, *availableShards, &nextSmallChunk, &candidateSiblings)) {
                // there isn't a chunk in this shard that can currently be moved and merged with one
                // of its siblings.
                continue;
            }

            // We have a chunk that can be moved&merged with at least one sibling. Choose one...
            tassert(8245213, "Chunk has too many siblings", candidateSiblings.size() <= 2);
            auto targetSibling = candidateSiblings.front();
            if (auto challenger = candidateSiblings.back(); targetSibling != challenger) {
                auto targetScore = _rankMergeableSibling(*nextSmallChunk, *targetSibling);
                auto challengerScore = _rankMergeableSibling(*nextSmallChunk, *challenger);
                if (challengerScore > targetScore ||
                    (challengerScore == targetScore &&
                     _shardInfos.at(challenger->shard).currentSizeBytes <
                         _shardInfos.at(targetSibling->shard).currentSizeBytes)) {
                    targetSibling = challenger;
                }
            }

            // ... then build up the migration request, marking the needed resources as busy.
            nextSmallChunk->busyInOperation = true;
            targetSibling->busyInOperation = true;
            availableShards->erase(nextSmallChunk->shard);
            availableShards->erase(targetSibling->shard);
            auto smallChunkVersion = getShardVersion(opCtx, nextSmallChunk->shard, _nss);
            _outstandingMigrations.emplace_back(nextSmallChunk, targetSibling);
            return _outstandingMigrations.back().asMigrateInfo(
                _uuid, _nss, smallChunkVersion.placementVersion(), _maxChunkSizeBytes);
        }

        return boost::none;
    }

    void applyActionResult(OperationContext* opCtx,
                           const BalancerStreamAction& action,
                           const BalancerStreamActionResponse& response) override {
        visit(OverloadedVisitor{
                  [&](const MigrateInfo& migrationAction) {
                      auto& migrationResponse = get<Status>(response);
                      auto match =
                          std::find_if(_outstandingMigrations.begin(),
                                       _outstandingMigrations.end(),
                                       [&migrationAction](const MoveAndMergeRequest& request) {
                                           return (migrationAction.minKey.woCompare(
                                                       request.getMigrationMinKey()) == 0);
                                       });
                      tassert(8245214,
                              "MigrationAction not found",
                              match != _outstandingMigrations.end());
                      MoveAndMergeRequest moveRequest(std::move(*match));
                      _outstandingMigrations.erase(match);

                      if (_aborted) {
                          return;
                      }

                      if (migrationResponse.isOK()) {
                          Grid::get(opCtx)
                              ->catalogCache()
                              ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                                  _nss, boost::none, moveRequest.getDestinationShard());

                          auto transferredAmount = moveRequest.getMovedDataSizeBytes();
                          tassert(8245215,
                                  "Unexpected amount of transferred data during chunk migration",
                                  transferredAmount <= _smallChunkSizeThresholdBytes);
                          _shardInfos.at(moveRequest.getSourceShard()).currentSizeBytes -=
                              transferredAmount;
                          _shardInfos.at(moveRequest.getDestinationShard()).currentSizeBytes +=
                              transferredAmount;
                          _shardProcessingOrder.sort(
                              [this](const ShardId& lhs, const ShardId& rhs) {
                                  return _shardInfos.at(lhs).currentSizeBytes >
                                      _shardInfos.at(rhs).currentSizeBytes;
                              });
                          _actionableMerges.push_back(std::move(moveRequest));
                          return;
                      }

                      LOGV2_DEBUG(6290000,
                                  1,
                                  "Migration failed during collection defragmentation",
                                  logAttrs(_nss),
                                  "uuid"_attr = _uuid,
                                  "currentPhase"_attr = getType(),
                                  "error"_attr = redact(migrationResponse));

                      moveRequest.chunkToMove->busyInOperation = false;
                      moveRequest.chunkToMergeWith->busyInOperation = false;

                      if (migrationResponse.code() == ErrorCodes::ChunkTooBig ||
                          migrationResponse.code() == ErrorCodes::ExceededMemoryLimit) {
                          // Never try moving this chunk again, it isn't actually small
                          _removeIteratorFromSmallChunks(moveRequest.chunkToMove,
                                                         moveRequest.chunkToMove->shard);
                          return;
                      }

                      if (isRetriableForDefragmentation(migrationResponse)) {
                          // The migration will be eventually retried
                          return;
                      }

                      const auto exceededTimeLimit = [&] {
                          // All errors thrown by the migration destination shard are converted
                          // into OperationFailed. Thus we need to inspect the error message to
                          // match the real error code.

                          // TODO SERVER-62990 introduce and propagate specific error code for
                          // migration failed due to range deletion pending
                          return migrationResponse == ErrorCodes::OperationFailed &&
                              migrationResponse.reason().find(ErrorCodes::errorString(
                                  ErrorCodes::ExceededTimeLimit)) != std::string::npos;
                      };

                      if (exceededTimeLimit()) {
                          // The migration failed because there is still a range deletion
                          // pending on the recipient.
                          moveRequest.chunkToMove->shardsToAvoid.emplace(
                              moveRequest.getDestinationShard());
                          return;
                      }

                      LOGV2_ERROR(6290001,
                                  "Encountered non-retriable error on migration during "
                                  "collection defragmentation",
                                  logAttrs(_nss),
                                  "uuid"_attr = _uuid,
                                  "currentPhase"_attr = getType(),
                                  "error"_attr = redact(migrationResponse));
                      _abort(DefragmentationPhaseEnum::kMergeAndMeasureChunks);
                  },
                  [&](const MergeInfo& mergeAction) {
                      auto& mergeResponse = get<Status>(response);
                      auto match = std::find_if(_outstandingMerges.begin(),
                                                _outstandingMerges.end(),
                                                [&mergeAction](const MoveAndMergeRequest& request) {
                                                    return mergeAction.chunkRange.containsKey(
                                                        request.getMigrationMinKey());
                                                });
                      tassert(8245216, "MergeAction not found", match != _outstandingMerges.end());
                      MoveAndMergeRequest mergeRequest(std::move(*match));
                      _outstandingMerges.erase(match);

                      auto onSuccess = [&] {
                          // The sequence is complete; update the state of the merged chunk...
                          auto& mergedChunk = mergeRequest.chunkToMergeWith;

                          Grid::get(opCtx)
                              ->catalogCache()
                              ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                                  _nss, boost::none, mergedChunk->shard);

                          auto& chunkToDelete = mergeRequest.chunkToMove;
                          mergedChunk->range = mergeRequest.asMergedRange();
                          if (mergedChunk->estimatedSizeBytes != kBigChunkMarker &&
                              chunkToDelete->estimatedSizeBytes != kBigChunkMarker) {
                              mergedChunk->estimatedSizeBytes += chunkToDelete->estimatedSizeBytes;
                          } else {
                              mergedChunk->estimatedSizeBytes = kBigChunkMarker;
                          }

                          mergedChunk->busyInOperation = false;
                          auto deletedChunkShard = chunkToDelete->shard;
                          // the lookup data structures...
                          _removeIteratorFromSmallChunks(chunkToDelete, deletedChunkShard);
                          if (mergedChunk->estimatedSizeBytes > _smallChunkSizeThresholdBytes) {
                              _removeIteratorFromSmallChunks(mergedChunk, mergedChunk->shard);
                          } else {
                              // Keep the list of small chunk iterators in the recipient sorted
                              auto match = _smallChunksByShard.find(mergedChunk->shard);
                              if (match != _smallChunksByShard.end()) {
                                  auto& [_, smallChunksInRecipient] = *match;
                                  smallChunksInRecipient.sort(compareChunkRangeInfoIterators);
                              }
                          }
                          //... and the collection
                          _collectionChunks.erase(chunkToDelete);
                      };

                      auto onRetriableError = [&] {
                          _actionableMerges.push_back(std::move(mergeRequest));
                      };

                      auto onNonRetriableError = [&]() {
                          _abort(DefragmentationPhaseEnum::kMergeAndMeasureChunks);
                      };

                      if (!_aborted) {
                          handleActionResult(opCtx,
                                             _nss,
                                             _uuid,
                                             getType(),
                                             mergeResponse,
                                             onSuccess,
                                             onRetriableError,
                                             onNonRetriableError);
                      }
                  },
                  [](const DataSizeInfo& dataSizeAction) {
                      uasserted(ErrorCodes::BadValue, "Unexpected action type");
                  },
                  [](const MergeAllChunksOnShardInfo& _) {
                      uasserted(ErrorCodes::BadValue, "Unexpected action type");
                  }},
              action);
    }

    bool isComplete() const override {
        return _smallChunksByShard.empty() && _outstandingMigrations.empty() &&
            _actionableMerges.empty() && _outstandingMerges.empty();
    }

    void userAbort() override {
        _abort(DefragmentationPhaseEnum::kFinished);
    }

    BSONObj reportProgress() const override {
        size_t numSmallChunks = 0;
        for (const auto& [shardId, smallChunks] : _smallChunksByShard) {
            numSmallChunks += smallChunks.size();
        }
        return BSON(kRemainingChunksToProcess << static_cast<long long>(numSmallChunks));
    }

private:
    // Internal representation of the chunk metadata required to generate a MoveAndMergeRequest
    struct ChunkRangeInfo {
        ChunkRangeInfo(ChunkRange&& range, const ShardId& shard, long long estimatedSizeBytes)
            : range(std::move(range)),
              shard(shard),
              estimatedSizeBytes(estimatedSizeBytes),
              busyInOperation(false) {}
        ChunkRange range;
        const ShardId shard;
        long long estimatedSizeBytes;
        bool busyInOperation;
        // Last time we failed to find a suitable destination shard due to temporary constraints
        boost::optional<Date_t> lastFailedAttemptTime;
        // Shards that still have a deletion pending for this range
        stdx::unordered_set<ShardId> shardsToAvoid;
    };

    struct ShardInfo {
        ShardInfo(uint64_t currentSizeBytes, bool draining)
            : currentSizeBytes(currentSizeBytes), draining(draining) {}

        bool isDraining() const {
            return draining;
        }

        uint64_t currentSizeBytes;
        const bool draining;
    };

    using ChunkRangeInfos = std::list<ChunkRangeInfo>;
    using ChunkRangeInfoIterator = ChunkRangeInfos::iterator;

    static bool compareChunkRangeInfoIterators(const ChunkRangeInfoIterator& lhs,
                                               const ChunkRangeInfoIterator& rhs) {
        // Small chunks are ordered by decreasing order of estimatedSizeBytes
        // except the ones that we failed to move due to temporary constraints that will be at the
        // end of the list ordered by last attempt time
        auto lhsLastFailureTime = lhs->lastFailedAttemptTime.value_or(Date_t::min());
        auto rhsLastFailureTime = rhs->lastFailedAttemptTime.value_or(Date_t::min());
        return std::tie(lhsLastFailureTime, lhs->estimatedSizeBytes) <
            std::tie(rhsLastFailureTime, rhs->estimatedSizeBytes);
    }

    // Helper class to generate the Migration and Merge actions required to join together the chunks
    // specified in the constructor
    struct MoveAndMergeRequest {
    public:
        MoveAndMergeRequest(const ChunkRangeInfoIterator& chunkToMove,
                            const ChunkRangeInfoIterator& chunkToMergeWith)
            : chunkToMove(chunkToMove),
              chunkToMergeWith(chunkToMergeWith),
              _isChunkToMergeLeftSibling(
                  chunkToMergeWith->range.getMax().woCompare(chunkToMove->range.getMin()) == 0) {}

        MigrateInfo asMigrateInfo(const UUID& collUuid,
                                  const NamespaceString& nss,
                                  const ChunkVersion& version,
                                  uint64_t maxChunkSizeBytes) const {
            return MigrateInfo(chunkToMergeWith->shard,
                               chunkToMove->shard,
                               nss,
                               collUuid,
                               chunkToMove->range.getMin(),
                               chunkToMove->range.getMax(),
                               version,
                               ForceJumbo::kDoNotForce,
                               maxChunkSizeBytes);
        }

        ChunkRange asMergedRange() const {
            return ChunkRange(_isChunkToMergeLeftSibling ? chunkToMergeWith->range.getMin()
                                                         : chunkToMove->range.getMin(),
                              _isChunkToMergeLeftSibling ? chunkToMove->range.getMax()
                                                         : chunkToMergeWith->range.getMax());
        }

        MergeInfo asMergeInfo(const UUID& collUuid,
                              const NamespaceString& nss,
                              const ChunkVersion& version) const {
            return MergeInfo(chunkToMergeWith->shard, nss, collUuid, version, asMergedRange());
        }

        const ShardId& getSourceShard() const {
            return chunkToMove->shard;
        }

        const ShardId& getDestinationShard() const {
            return chunkToMergeWith->shard;
        }

        const BSONObj& getMigrationMinKey() const {
            return chunkToMove->range.getMin();
        }

        int64_t getMovedDataSizeBytes() const {
            return chunkToMove->estimatedSizeBytes;
        }

        ChunkRangeInfoIterator chunkToMove;
        ChunkRangeInfoIterator chunkToMergeWith;

    private:
        bool _isChunkToMergeLeftSibling;
    };

    const NamespaceString _nss;

    const UUID _uuid;

    // The collection routing table - expressed in ChunkRangeInfo
    ChunkRangeInfos _collectionChunks;

    // List of indexes to elements in _collectionChunks that are eligible to be moved.
    std::map<ShardId, std::list<ChunkRangeInfoIterator>> _smallChunksByShard;

    stdx::unordered_map<ShardId, ShardInfo> _shardInfos;

    // Sorted list of shard IDs by decreasing current size (@see _shardInfos)
    std::list<ShardId> _shardProcessingOrder;

    // Set of attributes representing the currently active move&merge sequences
    std::list<MoveAndMergeRequest> _outstandingMigrations;
    std::list<MoveAndMergeRequest> _actionableMerges;
    std::list<MoveAndMergeRequest> _outstandingMerges;

    ZoneInfo _zoneInfo;

    const int64_t _smallChunkSizeThresholdBytes;

    const uint64_t _maxChunkSizeBytes;

    bool _aborted{false};

    DefragmentationPhaseEnum _nextPhase{DefragmentationPhaseEnum::kMergeChunks};

    MoveAndMergeChunksPhase(const NamespaceString& nss,
                            const UUID& uuid,
                            std::vector<ChunkType>&& collectionChunks,
                            stdx::unordered_map<ShardId, ShardInfo>&& shardInfos,
                            ZoneInfo&& collectionZones,
                            uint64_t smallChunkSizeThresholdBytes,
                            uint64_t maxChunkSizeBytes)
        : _nss(nss),
          _uuid(uuid),
          _collectionChunks(),
          _smallChunksByShard(),
          _shardInfos(std::move(shardInfos)),
          _shardProcessingOrder(),
          _outstandingMigrations(),
          _actionableMerges(),
          _outstandingMerges(),
          _zoneInfo(std::move(collectionZones)),
          _smallChunkSizeThresholdBytes(smallChunkSizeThresholdBytes),
          _maxChunkSizeBytes(maxChunkSizeBytes) {

        // Load the collection routing table in a std::list to ease later manipulation
        for (auto&& chunk : collectionChunks) {
            if (!chunk.getEstimatedSizeBytes().has_value()) {
                LOGV2_WARNING(
                    6172701,
                    "Chunk with no estimated size detected while building MoveAndMergeChunksPhase",
                    logAttrs(_nss),
                    "uuid"_attr = _uuid,
                    "range"_attr = chunk.getRange());
                _abort(DefragmentationPhaseEnum::kMergeAndMeasureChunks);
                return;
            }
            const uint64_t estimatedChunkSize = chunk.getEstimatedSizeBytes().value();
            _collectionChunks.emplace_back(chunk.getRange(), chunk.getShard(), estimatedChunkSize);
        }

        // Compose the index of small chunks
        for (auto chunkIt = _collectionChunks.begin(); chunkIt != _collectionChunks.end();
             ++chunkIt) {
            if (chunkIt->estimatedSizeBytes <= _smallChunkSizeThresholdBytes) {
                _smallChunksByShard[chunkIt->shard].emplace_back(chunkIt);
            }
        }
        // Each small chunk within a shard must be sorted by increasing chunk size
        for (auto& [_, smallChunksInShard] : _smallChunksByShard) {
            smallChunksInShard.sort(compareChunkRangeInfoIterators);
        }

        // Set the initial shard processing order
        for (const auto& [shardId, _] : _shardInfos) {
            _shardProcessingOrder.push_back(shardId);
        }
        _shardProcessingOrder.sort([this](const ShardId& lhs, const ShardId& rhs) {
            return _shardInfos.at(lhs).currentSizeBytes > _shardInfos.at(rhs).currentSizeBytes;
        });
    }

    void _abort(const DefragmentationPhaseEnum nextPhase) {
        _aborted = true;
        _nextPhase = nextPhase;
        _actionableMerges.clear();
        _smallChunksByShard.clear();
        _shardProcessingOrder.clear();
    }

    // Returns the list of siblings that are eligible to be move&merged with the specified chunk,
    // based  on shard zones and data capacity. (It does NOT take into account whether chunks are
    // currently involved in a move/merge operation).
    std::list<ChunkRangeInfoIterator> _getChunkSiblings(
        const ChunkRangeInfoIterator& chunkIt) const {
        std::list<ChunkRangeInfoIterator> siblings;
        auto canBeMoveAndMerged = [this](const ChunkRangeInfoIterator& chunkIt,
                                         const ChunkRangeInfoIterator& siblingIt) {
            auto onSameZone = _zoneInfo.getZoneForRange(chunkIt->range) ==
                _zoneInfo.getZoneForRange(siblingIt->range);
            auto destinationAvailable = chunkIt->shard == siblingIt->shard ||
                !_shardInfos.at(siblingIt->shard).isDraining();
            return (onSameZone && destinationAvailable);
        };

        if (auto rightSibling = std::next(chunkIt);
            rightSibling != _collectionChunks.end() && canBeMoveAndMerged(chunkIt, rightSibling)) {
            siblings.push_back(rightSibling);
        }
        if (chunkIt != _collectionChunks.begin()) {
            auto leftSibling = std::prev(chunkIt);
            if (canBeMoveAndMerged(chunkIt, leftSibling)) {
                siblings.push_back(leftSibling);
            }
        }
        return siblings;
    }

    // Computes whether there is a chunk in the specified shard that can be moved&merged with one or
    // both of its siblings. Chunks/siblings that are currently being moved/merged are not eligible.
    //
    // The function also clears the internal state from elements that cannot be processed by the
    // phase (chunks with no siblings, shards with no small chunks).
    //
    // Returns true on success (storing the related info in nextSmallChunk + smallChunkSiblings),
    // false otherwise.
    bool _findNextSmallChunkInShard(const ShardId& shard,
                                    const stdx::unordered_set<ShardId>& availableShards,
                                    ChunkRangeInfoIterator* nextSmallChunk,
                                    std::list<ChunkRangeInfoIterator>* smallChunkSiblings) {
        auto matchingShardInfo = _smallChunksByShard.find(shard);
        if (matchingShardInfo == _smallChunksByShard.end()) {
            return false;
        }

        smallChunkSiblings->clear();
        auto& smallChunksInShard = matchingShardInfo->second;
        for (auto candidateIt = smallChunksInShard.begin();
             candidateIt != smallChunksInShard.end();) {
            if ((*candidateIt)->busyInOperation) {
                ++candidateIt;
                continue;
            }
            auto candidateSiblings = _getChunkSiblings(*candidateIt);
            if (candidateSiblings.empty()) {
                // The current chunk cannot be processed by the algorithm - remove it.
                candidateIt = smallChunksInShard.erase(candidateIt);
                continue;
            }

            size_t siblingsDiscardedDueToRangeDeletion = 0;

            for (const auto& sibling : candidateSiblings) {
                if (sibling->busyInOperation || !availableShards.count(sibling->shard)) {
                    continue;
                }
                if ((*candidateIt)->shardsToAvoid.count(sibling->shard)) {
                    ++siblingsDiscardedDueToRangeDeletion;
                    continue;
                }
                smallChunkSiblings->push_back(sibling);
            }

            if (!smallChunkSiblings->empty()) {
                *nextSmallChunk = *candidateIt;
                return true;
            }


            if (siblingsDiscardedDueToRangeDeletion == candidateSiblings.size()) {
                // All the siblings have been discarded because an overlapping range deletion is
                // still pending on the destination shard.
                if (!(*candidateIt)->lastFailedAttemptTime) {
                    // This is the first time we discard this chunk due to overlapping range
                    // deletions pending. Enqueue it back on the list so we will try to move it
                    // again when we will have drained all the other chunks for this shard.
                    LOGV2_DEBUG(6290002,
                                1,
                                "Postponing small chunk processing due to pending range deletion "
                                "on recipient shard(s)",
                                logAttrs(_nss),
                                "uuid"_attr = _uuid,
                                "range"_attr = (*candidateIt)->range,
                                "estimatedSizeBytes"_attr = (*candidateIt)->estimatedSizeBytes,
                                "numCandidateSiblings"_attr = candidateSiblings.size());
                    (*candidateIt)->lastFailedAttemptTime = Date_t::now();
                    (*candidateIt)->shardsToAvoid.clear();
                    smallChunksInShard.emplace_back(*candidateIt);
                } else {
                    LOGV2(6290003,
                          "Discarding small chunk due to pending range deletion on recipient shard",
                          logAttrs(_nss),
                          "uuid"_attr = _uuid,
                          "range"_attr = (*candidateIt)->range,
                          "estimatedSizeBytes"_attr = (*candidateIt)->estimatedSizeBytes,
                          "numCandidateSiblings"_attr = candidateSiblings.size(),
                          "lastFailedAttempt"_attr = (*candidateIt)->lastFailedAttemptTime);
                }
                candidateIt = smallChunksInShard.erase(candidateIt);
                continue;
            }

            ++candidateIt;
        }
        // No candidate could be found - clear the shard entry if needed
        if (smallChunksInShard.empty()) {
            _smallChunksByShard.erase(matchingShardInfo);
        }
        return false;
    }

    uint32_t _rankMergeableSibling(const ChunkRangeInfo& chunkTobeMovedAndMerged,
                                   const ChunkRangeInfo& mergeableSibling) {
        static constexpr uint32_t kNoMoveRequired = 1 << 3;
        static constexpr uint32_t kConvenientMove = 1 << 2;
        static constexpr uint32_t kMergeSolvesTwoPendingChunks = 1 << 1;
        static constexpr uint32_t kMergeSolvesOnePendingChunk = 1;
        uint32_t ranking = 0;
        if (chunkTobeMovedAndMerged.shard == mergeableSibling.shard) {
            ranking += kNoMoveRequired;
        } else if (chunkTobeMovedAndMerged.estimatedSizeBytes <
                   mergeableSibling.estimatedSizeBytes) {
            ranking += kConvenientMove;
        }
        auto estimatedMergedSize = (chunkTobeMovedAndMerged.estimatedSizeBytes == kBigChunkMarker ||
                                    mergeableSibling.estimatedSizeBytes == kBigChunkMarker)
            ? kBigChunkMarker
            : chunkTobeMovedAndMerged.estimatedSizeBytes + mergeableSibling.estimatedSizeBytes;
        if (estimatedMergedSize > _smallChunkSizeThresholdBytes) {
            ranking += mergeableSibling.estimatedSizeBytes < _smallChunkSizeThresholdBytes
                ? kMergeSolvesTwoPendingChunks
                : kMergeSolvesOnePendingChunk;
        }

        return ranking;
    }

    void _removeIteratorFromSmallChunks(const ChunkRangeInfoIterator& chunkIt,
                                        const ShardId& parentShard) {
        auto matchingShardIt = _smallChunksByShard.find(parentShard);
        if (matchingShardIt == _smallChunksByShard.end()) {
            return;
        }
        auto& smallChunksInShard = matchingShardIt->second;
        auto match = std::find(smallChunksInShard.begin(), smallChunksInShard.end(), chunkIt);
        if (match == smallChunksInShard.end()) {
            return;
        }
        smallChunksInShard.erase(match);
        if (smallChunksInShard.empty()) {
            _smallChunksByShard.erase(parentShard);
        }
    }
};

class MergeChunksPhase : public DefragmentationPhase {
public:
    static std::unique_ptr<MergeChunksPhase> build(OperationContext* opCtx,
                                                   const CollectionType& coll) {
        auto collectionChunks = getCollectionChunks(opCtx, coll);
        const auto collectionZones = getCollectionZones(opCtx, coll);

        // Find ranges of mergeable chunks
        stdx::unordered_map<ShardId, std::vector<ChunkRange>> unmergedRangesByShard;
        while (!collectionChunks.empty()) {
            auto upperRangeBound = std::prev(collectionChunks.cend());
            auto lowerRangeBound = upperRangeBound;
            while (lowerRangeBound != collectionChunks.cbegin() &&
                   areMergeable(*std::prev(lowerRangeBound), *lowerRangeBound, collectionZones)) {
                --lowerRangeBound;
            }
            if (lowerRangeBound != upperRangeBound) {
                unmergedRangesByShard[upperRangeBound->getShard()].emplace_back(
                    lowerRangeBound->getMin(), upperRangeBound->getMax());
            }

            collectionChunks.erase(lowerRangeBound, std::next(upperRangeBound));
        }
        return std::unique_ptr<MergeChunksPhase>(
            new MergeChunksPhase(coll.getNss(), coll.getUuid(), std::move(unmergedRangesByShard)));
    }

    DefragmentationPhaseEnum getType() const override {
        return DefragmentationPhaseEnum::kMergeChunks;
    }

    DefragmentationPhaseEnum getNextPhase() const override {
        return _nextPhase;
    }

    boost::optional<BalancerStreamAction> popNextStreamableAction(
        OperationContext* opCtx) override {
        if (_unmergedRangesByShard.empty()) {
            return boost::none;
        }

        auto it = _shardToProcess ? _unmergedRangesByShard.find(*_shardToProcess)
                                  : _unmergedRangesByShard.begin();

        tassert(8245217,
                str::stream() << "Shard to process not found in unmerged ranges. ShardId: "
                              << *_shardToProcess,
                it != _unmergedRangesByShard.end());

        auto& [shardId, unmergedRanges] = *it;
        tassert(8245218,
                str::stream() << "Unmerged ranges is empty. ShardId: " << *_shardToProcess,
                !unmergedRanges.empty());
        auto shardVersion = getShardVersion(opCtx, shardId, _nss);
        const auto& rangeToMerge = unmergedRanges.back();
        boost::optional<BalancerStreamAction> nextAction = boost::optional<BalancerStreamAction>(
            MergeInfo(shardId, _nss, _uuid, shardVersion.placementVersion(), rangeToMerge));
        unmergedRanges.pop_back();
        ++_outstandingActions;
        if (unmergedRanges.empty()) {
            it = _unmergedRangesByShard.erase(it, std::next(it));
        } else {
            ++it;
        }
        if (it != _unmergedRangesByShard.end()) {
            _shardToProcess = it->first;
        } else {
            _shardToProcess = boost::none;
        }

        return nextAction;
    }

    boost::optional<MigrateInfo> popNextMigration(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* availableShards) override {
        return boost::none;
    }

    void applyActionResult(OperationContext* opCtx,
                           const BalancerStreamAction& action,
                           const BalancerStreamActionResponse& response) override {
        ScopeGuard scopedGuard([&] { --_outstandingActions; });
        if (_aborted) {
            return;
        }
        visit(OverloadedVisitor{[&](const MergeInfo& mergeAction) {
                                    auto& mergeResponse = get<Status>(response);
                                    auto onSuccess = [] {
                                    };
                                    auto onRetriableError = [&] {
                                        _unmergedRangesByShard[mergeAction.shardId].emplace_back(
                                            mergeAction.chunkRange);
                                    };
                                    auto onNonretriableError = [this] {
                                        _abort(getType());
                                    };
                                    handleActionResult(opCtx,
                                                       _nss,
                                                       _uuid,
                                                       getType(),
                                                       mergeResponse,
                                                       onSuccess,
                                                       onRetriableError,
                                                       onNonretriableError);
                                },
                                [](const DataSizeInfo& _) {
                                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                                },
                                [](const MigrateInfo& _) {
                                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                                },
                                [](const MergeAllChunksOnShardInfo& _) {
                                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                                }},
              action);
    }

    bool isComplete() const override {
        return _unmergedRangesByShard.empty() && _outstandingActions == 0;
    }

    void userAbort() override {
        _abort(DefragmentationPhaseEnum::kFinished);
    }

    BSONObj reportProgress() const override {
        size_t rangesToMerge = 0;
        for (const auto& [_, unmergedRanges] : _unmergedRangesByShard) {
            rangesToMerge += unmergedRanges.size();
        }
        auto remainingRangesToProcess =
            static_cast<long long>(_outstandingActions) + static_cast<long long>(rangesToMerge);

        return BSON(kRemainingChunksToProcess << remainingRangesToProcess);
    }

private:
    MergeChunksPhase(const NamespaceString& nss,
                     const UUID& uuid,
                     stdx::unordered_map<ShardId, std::vector<ChunkRange>>&& unmergedRangesByShard)
        : _nss(nss), _uuid(uuid), _unmergedRangesByShard(std::move(unmergedRangesByShard)) {}

    void _abort(const DefragmentationPhaseEnum nextPhase) {
        _aborted = true;
        _nextPhase = nextPhase;
        _unmergedRangesByShard.clear();
    }

    const NamespaceString _nss;
    const UUID _uuid;
    stdx::unordered_map<ShardId, std::vector<ChunkRange>> _unmergedRangesByShard;
    boost::optional<ShardId> _shardToProcess;
    size_t _outstandingActions{0};
    bool _aborted{false};
    DefragmentationPhaseEnum _nextPhase{DefragmentationPhaseEnum::kFinished};
};

}  // namespace

void BalancerDefragmentationPolicy::startCollectionDefragmentations(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_stateMutex);

    // Fetch all collections with `defragmentCollection` flag enabled
    static const auto query =
        BSON(CollectionType::kDefragmentCollectionFieldName
             << true << CollectionType::kUnsplittableFieldName << BSON("$ne" << true));

    const auto& configShard = ShardingCatalogManager::get(opCtx)->localConfigShard();
    const auto& collDocs = uassertStatusOK(configShard->exhaustiveFindOnConfig(
                                               opCtx,
                                               ReadPreferenceSetting(ReadPreference::Nearest),
                                               repl::ReadConcernLevel::kMajorityReadConcern,
                                               NamespaceString::kConfigsvrCollectionsNamespace,
                                               query,
                                               BSONObj(),
                                               boost::none))
                               .docs;

    for (const BSONObj& obj : collDocs) {
        const CollectionType coll{obj};
        if (_defragmentationStates.contains(coll.getUuid())) {
            continue;
        }
        _initializeCollectionState(lk, opCtx, coll);
    }
    _onStateUpdated();
}

void BalancerDefragmentationPolicy::abortCollectionDefragmentation(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    auto coll =
        ShardingCatalogManager::get(opCtx)->localCatalogClient()->getCollection(opCtx, nss, {});
    if (coll.getDefragmentCollection()) {
        if (_defragmentationStates.contains(coll.getUuid())) {
            // Notify phase to abort current phase
            _defragmentationStates.at(coll.getUuid())->userAbort();
            _onStateUpdated();
        }
        _persistPhaseUpdate(opCtx, DefragmentationPhaseEnum::kFinished, coll.getUuid());
    }
}

void BalancerDefragmentationPolicy::interruptAllDefragmentations() {
    stdx::lock_guard<Latch> lk(_stateMutex);
    _defragmentationStates.clear();
}

bool BalancerDefragmentationPolicy::isDefragmentingCollection(const UUID& uuid) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    return _defragmentationStates.contains(uuid);
}

BSONObj BalancerDefragmentationPolicy::reportProgressOn(const UUID& uuid) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    auto match = _defragmentationStates.find(uuid);
    if (match == _defragmentationStates.end() || !match->second) {
        return BSON(kCurrentPhase << kNoPhase);
    }
    const auto& collDefragmentationPhase = match->second;
    return BSON(
        kCurrentPhase << DefragmentationPhase_serializer(collDefragmentationPhase->getType())
                      << kProgress << collDefragmentationPhase->reportProgress());
}

MigrateInfoVector BalancerDefragmentationPolicy::selectChunksToMove(
    OperationContext* opCtx, stdx::unordered_set<ShardId>* availableShards) {

    MigrateInfoVector chunksToMove;
    {
        stdx::lock_guard<Latch> lk(_stateMutex);

        std::vector<UUID> collectionUUIDs;
        collectionUUIDs.reserve(_defragmentationStates.size());
        for (const auto& defragState : _defragmentationStates) {
            collectionUUIDs.push_back(defragState.first);
        }

        auto client = opCtx->getClient();
        std::shuffle(collectionUUIDs.begin(), collectionUUIDs.end(), client->getPrng().urbg());

        auto popCollectionUUID =
            [&](std::vector<UUID>::iterator elemIt) -> std::vector<UUID>::iterator {
            if (std::next(elemIt) == collectionUUIDs.end()) {
                return collectionUUIDs.erase(elemIt);
            }

            *elemIt = std::move(collectionUUIDs.back());
            collectionUUIDs.pop_back();
            return elemIt;
        };

        while (!collectionUUIDs.empty()) {
            for (auto it = collectionUUIDs.begin(); it != collectionUUIDs.end();) {
                const auto& collUUID = *it;

                if (availableShards->size() == 0) {
                    return chunksToMove;
                }

                try {
                    auto defragStateIt = _defragmentationStates.find(collUUID);
                    if (defragStateIt == _defragmentationStates.end()) {
                        it = popCollectionUUID(it);
                        continue;
                    };

                    auto& collDefragmentationPhase = defragStateIt->second;
                    if (!collDefragmentationPhase) {
                        _defragmentationStates.erase(defragStateIt);
                        it = popCollectionUUID(it);
                        continue;
                    }
                    auto actionableMigration =
                        collDefragmentationPhase->popNextMigration(opCtx, availableShards);
                    if (!actionableMigration.has_value()) {
                        it = popCollectionUUID(it);
                        continue;
                    }
                    chunksToMove.push_back(std::move(*actionableMigration));
                    ++it;
                } catch (DBException& e) {
                    // Catch getCollection and getShardVersion errors. Should only occur if
                    // collection has been removed.
                    LOGV2_ERROR(6172700,
                                "Error while getting next migration",
                                "uuid"_attr = collUUID,
                                "error"_attr = redact(e));
                    _defragmentationStates.erase(collUUID);
                    it = popCollectionUUID(it);
                }
            }
        }
    }

    if (chunksToMove.empty()) {
        // If the policy cannot produce new migrations even in absence of temporary constraints, it
        // is possible that some streaming actions must be processed first. Notify an update of the
        // internal state to make it happen.
        _onStateUpdated();
    }
    return chunksToMove;
}

StringData BalancerDefragmentationPolicy::getName() const {
    return StringData(kPolicyName);
}

boost::optional<BalancerStreamAction> BalancerDefragmentationPolicy::getNextStreamingAction(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    // Visit the defrag state in round robin fashion starting from a random one
    auto stateIt = [&] {
        auto it = _defragmentationStates.begin();
        if (_defragmentationStates.size() > 1) {
            auto client = opCtx->getClient();
            std::advance(it, client->getPrng().nextInt32(_defragmentationStates.size()));
        }
        return it;
    }();

    for (auto stateToVisit = _defragmentationStates.size(); stateToVisit != 0; --stateToVisit) {
        try {
            _advanceToNextActionablePhase(opCtx, stateIt->first);
            auto& currentCollectionDefragmentationState = stateIt->second;
            if (currentCollectionDefragmentationState) {
                // Get next action
                auto nextAction =
                    currentCollectionDefragmentationState->popNextStreamableAction(opCtx);
                if (nextAction) {
                    return nextAction;
                }
                ++stateIt;
            } else {
                stateIt = _defragmentationStates.erase(stateIt, std::next(stateIt));
            }
        } catch (DBException& e) {
            // Catch getCollection and getShardVersion errors. Should only occur if collection has
            // been removed.
            LOGV2_ERROR(6153301,
                        "Error while getting next defragmentation action",
                        "uuid"_attr = stateIt->first,
                        "error"_attr = redact(e));
            stateIt = _defragmentationStates.erase(stateIt, std::next(stateIt));
        }

        if (stateIt == _defragmentationStates.end()) {
            stateIt = _defragmentationStates.begin();
        }
    }

    return boost::none;
}

bool BalancerDefragmentationPolicy::_advanceToNextActionablePhase(OperationContext* opCtx,
                                                                  const UUID& collUuid) {
    auto& currentPhase = _defragmentationStates.at(collUuid);
    auto phaseTransitionNeeded = [&currentPhase] {
        return currentPhase && currentPhase->isComplete() &&
            MONGO_likely(!skipDefragmentationPhaseTransition.shouldFail());
    };
    bool advanced = false;
    boost::optional<CollectionType> coll(boost::none);
    while (phaseTransitionNeeded()) {
        if (!coll) {
            coll = ShardingCatalogManager::get(opCtx)->localCatalogClient()->getCollection(
                opCtx, collUuid);
        }
        currentPhase = _transitionPhases(opCtx, *coll, currentPhase->getNextPhase());
        advanced = true;
    }
    return advanced;
}

void BalancerDefragmentationPolicy::applyActionResult(
    OperationContext* opCtx,
    const BalancerStreamAction& action,
    const BalancerStreamActionResponse& response) {
    {
        stdx::lock_guard<Latch> lk(_stateMutex);
        DefragmentationPhase* targetState = nullptr;
        visit(OverloadedVisitor{[&](const MergeInfo& act) {
                                    if (_defragmentationStates.contains(act.uuid)) {
                                        targetState = _defragmentationStates.at(act.uuid).get();
                                    }
                                },
                                [&](const DataSizeInfo& act) {
                                    if (_defragmentationStates.contains(act.uuid)) {
                                        targetState = _defragmentationStates.at(act.uuid).get();
                                    }
                                },
                                [&](const MigrateInfo& act) {
                                    if (_defragmentationStates.contains(act.uuid)) {
                                        targetState = _defragmentationStates.at(act.uuid).get();
                                    }
                                },
                                [](const MergeAllChunksOnShardInfo& _) {
                                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                                }},
              action);

        if (targetState) {
            targetState->applyActionResult(opCtx, action, response);
        }
    }
    _onStateUpdated();
}

std::unique_ptr<DefragmentationPhase> BalancerDefragmentationPolicy::_transitionPhases(
    OperationContext* opCtx,
    const CollectionType& coll,
    DefragmentationPhaseEnum nextPhase,
    bool shouldPersistPhase) {
    std::unique_ptr<DefragmentationPhase> nextPhaseObject(nullptr);

    try {
        if (shouldPersistPhase) {
            _persistPhaseUpdate(opCtx, nextPhase, coll.getUuid());
        }
        switch (nextPhase) {
            case DefragmentationPhaseEnum::kMergeAndMeasureChunks:
                nextPhaseObject = MergeAndMeasureChunksPhase::build(opCtx, coll);
                break;
            case DefragmentationPhaseEnum::kMoveAndMergeChunks: {
                auto collectionShardStats =
                    uassertStatusOK(_clusterStats->getCollStats(opCtx, coll.getNss()));
                nextPhaseObject =
                    MoveAndMergeChunksPhase::build(opCtx, coll, std::move(collectionShardStats));
            } break;
            case DefragmentationPhaseEnum::kMergeChunks:
                nextPhaseObject = MergeChunksPhase::build(opCtx, coll);
                break;
            case DefragmentationPhaseEnum::kFinished:
            default:  // Exit defragmentation in case of unexpected phase
                _clearDefragmentationState(opCtx, coll.getUuid());
                break;
        }
        afterBuildingNextDefragmentationPhase.pauseWhileSet();
        LOGV2(6172702,
              "Collection defragmentation transitioned to new phase",
              logAttrs(coll.getNss()),
              "phase"_attr = nextPhaseObject
                  ? DefragmentationPhase_serializer(nextPhaseObject->getType())
                  : kNoPhase,
              "details"_attr = nextPhaseObject ? nextPhaseObject->reportProgress() : BSONObj());
    } catch (const DBException& e) {
        LOGV2_ERROR(6153101,
                    "Error while building defragmentation phase on collection",
                    logAttrs(coll.getNss()),
                    "uuid"_attr = coll.getUuid(),
                    "phase"_attr = nextPhase,
                    "error"_attr = e);
    }
    return nextPhaseObject;
}

void BalancerDefragmentationPolicy::_initializeCollectionState(WithLock,
                                                               OperationContext* opCtx,
                                                               const CollectionType& coll) {
    if (MONGO_unlikely(skipDefragmentationPhaseTransition.shouldFail())) {
        return;
    }
    auto phaseToBuild = coll.getDefragmentationPhase()
        ? coll.getDefragmentationPhase().value()
        : DefragmentationPhaseEnum::kMergeAndMeasureChunks;
    auto collectionPhase =
        _transitionPhases(opCtx, coll, phaseToBuild, !coll.getDefragmentationPhase().has_value());
    while (collectionPhase && collectionPhase->isComplete() &&
           MONGO_likely(!skipDefragmentationPhaseTransition.shouldFail())) {
        collectionPhase = _transitionPhases(opCtx, coll, collectionPhase->getNextPhase());
    }
    if (collectionPhase) {
        auto [_, inserted] =
            _defragmentationStates.insert_or_assign(coll.getUuid(), std::move(collectionPhase));
        dassert(inserted);
    }
}

void BalancerDefragmentationPolicy::_persistPhaseUpdate(OperationContext* opCtx,
                                                        DefragmentationPhaseEnum phase,
                                                        const UUID& uuid) {
    DBDirectClient dbClient(opCtx);
    write_ops::UpdateCommandRequest updateOp(CollectionType::ConfigNS);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kUuidFieldName << uuid));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$set" << BSON(CollectionType::kDefragmentationPhaseFieldName
                                << DefragmentationPhase_serializer(phase)))));
        return entry;
    }()});
    auto response = write_ops::checkWriteErrors(dbClient.update(updateOp));
    uassert(ErrorCodes::NoMatchingDocument,
            "Collection {} not found while persisting phase change"_format(uuid.toString()),
            response.getN() > 0);
    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(
        opCtx, latestOpTime, WriteConcerns::kMajorityWriteConcernShardingTimeout, &ignoreResult));
}

void BalancerDefragmentationPolicy::_clearDefragmentationState(OperationContext* opCtx,
                                                               const UUID& uuid) {
    DBDirectClient dbClient(opCtx);

    // Clear datasize estimates from chunks
    write_ops::checkWriteErrors(dbClient.update(write_ops::UpdateCommandRequest(
        ChunkType::ConfigNS, {[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(BSON(CollectionType::kUuidFieldName << uuid));
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                BSON("$unset" << BSON(ChunkType::estimatedSizeBytes.name() << ""))));
            entry.setMulti(true);
            return entry;
        }()})));

    // Clear defragmentation phase and defragmenting flag from collection
    write_ops::checkWriteErrors(dbClient.update(write_ops::UpdateCommandRequest(
        CollectionType::ConfigNS, {[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(BSON(CollectionType::kUuidFieldName << uuid));
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON(
                "$unset" << BSON(CollectionType::kDefragmentCollectionFieldName
                                 << "" << CollectionType::kDefragmentationPhaseFieldName << ""))));
            return entry;
        }()})));

    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(
        opCtx, latestOpTime, WriteConcerns::kMajorityWriteConcernShardingTimeout, &ignoreResult));
}

}  // namespace mongo

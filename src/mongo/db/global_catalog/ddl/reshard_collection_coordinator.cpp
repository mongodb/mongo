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


#include "mongo/db/global_catalog/ddl/reshard_collection_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

ReshardCollectionCoordinator::ReshardCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                                           const BSONObj& initialState)
    : ReshardCollectionCoordinator(service, initialState, true /* persistCoordinatorDocument */) {}

ReshardCollectionCoordinator::ReshardCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                                           const BSONObj& initialState,
                                                           bool persistCoordinatorDocument)
    : RecoverableShardingDDLCoordinator(service, "ReshardCollectionCoordinator", initialState),
      _request(_doc.getReshardCollectionRequest()) {}

void ReshardCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = ReshardCollectionCoordinatorDocument::parse(
        doc, IDLParserContext("ReshardCollectionCoordinatorDocument"));

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another reshard collection with different arguments is already running for the same "
            "namespace",
            SimpleBSONObjComparator::kInstance.evaluate(
                _request.toBSON() == otherDoc.getReshardCollectionRequest().toBSON()));
}

void ReshardCollectionCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

ExecutorFuture<void> ReshardCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(Phase::kReshard, [this, anchor = shared_from_this()](auto* opCtx) {
            {
                // Resharding on view is never allowed. However, historically, resharding doesn't
                // fail with "CommandNotSupportedOnView" when running on a views but with
                // "IllegalOperation". We therefore allow views here so that further checks can
                // allow the back-compatible error.
                auto coll = acquireCollectionOrView(
                    opCtx,
                    CollectionOrViewAcquisitionRequest::fromOpCtx(
                        opCtx, nss(), _doc.getCollectionUUID(), AcquisitionPrerequisites::kRead),
                    MODE_IS);
            }

            const auto cmOld = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx,
                                                                                        nss()));

            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Collection '" << nss().toStringForErrorMsg()
                                  << "' not found in cluster catalog",
                    cmOld.hasRoutingTable());

            StateDoc newDoc(_doc);
            newDoc.setOldShardKey(cmOld.getShardKeyPattern().getKeyPattern().toBSON());
            newDoc.setOldCollectionUUID(cmOld.getUUID());
            _updateStateDocument(opCtx, std::move(newDoc));

            ConfigsvrReshardCollection configsvrReshardCollection(nss(), _doc.getKey());
            configsvrReshardCollection.setDbName(nss().dbName());
            configsvrReshardCollection.setUnique(_doc.getUnique());
            configsvrReshardCollection.setCollation(_doc.getCollation());
            configsvrReshardCollection.set_presetReshardedChunks(_doc.get_presetReshardedChunks());
            configsvrReshardCollection.setZones(_doc.getZones());
            configsvrReshardCollection.setNumInitialChunks(_doc.getNumInitialChunks());
            configsvrReshardCollection.setRecipientOplogBatchTaskCount(
                _doc.getRecipientOplogBatchTaskCount());

            // TODO SERVER-92437 ensure this behavior is safe during FCV upgrade/downgrade
            if (!resharding::gFeatureFlagReshardingRelaxedMode.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                uassert(ErrorCodes::InvalidOptions,
                        "Relaxed mode is not enabled, reject relaxed parameter",
                        !_doc.getRelaxed().has_value());
            }

            if (!resharding::gFeatureFlagMoveCollection.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
                !resharding::gFeatureFlagUnshardCollection.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                uassert(ErrorCodes::InvalidOptions,
                        "Feature flag move collection or unshard collection is not enabled, reject "
                        "provenance",
                        !_doc.getProvenance().has_value() ||
                            _doc.getProvenance().get() ==
                                ReshardingProvenanceEnum::kReshardCollection);
            }

            configsvrReshardCollection.setRelaxed(_doc.getRelaxed());
            configsvrReshardCollection.setShardDistribution(_doc.getShardDistribution());
            configsvrReshardCollection.setForceRedistribution(_doc.getForceRedistribution());
            configsvrReshardCollection.setReshardingUUID(_doc.getReshardingUUID());

            resharding::validatePerformVerification(VersionContext::getDecoration(opCtx),
                                                    _doc.getPerformVerification());
            configsvrReshardCollection.setPerformVerification(_doc.getPerformVerification());

            auto provenance = _doc.getProvenance();
            if (resharding::isMoveCollection(provenance)) {
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream()
                            << "MoveCollection can only be called on an unsharded collection.",
                        !cmOld.isSharded());
            } else if (provenance &&
                       provenance.get() == ReshardingProvenanceEnum::kUnshardCollection) {
                // If the collection is already unsharded, this request should be a no-op. Check
                // that the user didn't specify a "to" shard other than the shard the collection
                // lives on - if it is different, return an error.
                if (!cmOld.isSharded()) {
                    if (_doc.getShardDistribution()) {
                        std::set<ShardId> currentShards;
                        cmOld.getAllShardIds(&currentShards);
                        const auto toShard = _doc.getShardDistribution().get().front().getShard();
                        uassert(ErrorCodes::NamespaceNotSharded,
                                "Collection is already unsharded. Call moveCollection to move this "
                                "collection to a different shard.",
                                currentShards.find(toShard) != currentShards.end());
                    }

                    return;
                }

                // Pick the "to" shard if the client did not specify one.
                if (!_doc.getShardDistribution()) {
                    auto toShard = sharding_util::selectLeastLoadedNonDrainingShard(opCtx);
                    mongo::ShardKeyRange destinationRange(toShard);
                    destinationRange.setMin(cluster::unsplittable::kUnsplittableCollectionMinKey);
                    destinationRange.setMax(cluster::unsplittable::kUnsplittableCollectionMaxKey);
                    std::vector<mongo::ShardKeyRange> distribution = {destinationRange};
                    configsvrReshardCollection.setShardDistribution(distribution);
                }
            } else {
                uassert(ErrorCodes::NamespaceNotSharded,
                        "Collection has to be a sharded collection.",
                        cmOld.isSharded());

                if (_doc.getForceRedistribution() && *_doc.getForceRedistribution()) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream()
                                << "The new shard key must be the same as the original shard key "
                                   "when using the forceRedistribution option. The "
                                   "forceRedistribution option is meant for redistributing the "
                                   "collection to a different set of shards.",
                            cmOld.getShardKeyPattern().isShardKey(_doc.getKey()));
                }
            }

            configsvrReshardCollection.setProvenance(provenance);
            configsvrReshardCollection.setDemoMode(_doc.getDemoMode());
            generic_argument_util::setMajorityWriteConcern(configsvrReshardCollection,
                                                           &opCtx->getWriteConcern());

            const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            const auto cmdResponse = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        DatabaseName::kAdmin,
                                        configsvrReshardCollection.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
        }));
}

}  // namespace mongo

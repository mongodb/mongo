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


#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/reshard_collection_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

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
        IDLParserContext("ReshardCollectionCoordinatorDocument"), doc);

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
        .then(_buildPhaseHandler(Phase::kReshard, [this, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            {
                AutoGetCollection coll{opCtx,
                                       nss(),
                                       MODE_IS,
                                       AutoGetCollection::Options{}
                                           .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                           .expectedUUID(_doc.getCollectionUUID())};
            }

            const auto cmOld =
                uassertStatusOK(
                    Grid::get(opCtx)
                        ->catalogCache()
                        ->getTrackedCollectionRoutingInfoWithPlacementRefresh(opCtx, nss()))
                    .cm;

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

            if (!resharding::gFeatureFlagReshardingImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility)) {
                uassert(
                    ErrorCodes::InvalidOptions,
                    "Resharding improvements is not enabled, reject shardDistribution parameter",
                    !_doc.getShardDistribution().has_value());
                uassert(
                    ErrorCodes::InvalidOptions,
                    "Resharding improvements is not enabled, reject forceRedistribution parameter",
                    !_doc.getForceRedistribution().has_value());
                uassert(ErrorCodes::InvalidOptions,
                        "Resharding improvements is not enabled, reject reshardingUUID parameter",
                        !_doc.getReshardingUUID().has_value());
                if (!resharding::gFeatureFlagMoveCollection.isEnabled(
                        serverGlobalParams.featureCompatibility) ||
                    !resharding::gFeatureFlagUnshardCollection.isEnabled(
                        serverGlobalParams.featureCompatibility)) {
                    uassert(ErrorCodes::InvalidOptions,
                            "Feature flag move collection or unshard collection is not enabled, "
                            "reject provenance parameter",
                            !_doc.getProvenance().has_value());
                }
            }
            configsvrReshardCollection.setShardDistribution(_doc.getShardDistribution());
            configsvrReshardCollection.setForceRedistribution(_doc.getForceRedistribution());
            configsvrReshardCollection.setReshardingUUID(_doc.getReshardingUUID());

            auto provenance = _doc.getProvenance();
            if (provenance && provenance.get() == ProvenanceEnum::kMoveCollection) {
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream()
                            << "MoveCollection can only be called on an unsharded collection.",
                        !cmOld.isSharded() && cmOld.hasRoutingTable());
            } else {
                uassert(ErrorCodes::NamespaceNotSharded,
                        "Collection has to be a sharded collection.",
                        cmOld.isSharded());
            }

            configsvrReshardCollection.setProvenance(provenance);

            const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            const auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                DatabaseName::kAdmin,
                CommandHelpers::appendMajorityWriteConcern(configsvrReshardCollection.toBSON({}),
                                                           opCtx->getWriteConcern()),
                Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(std::move(cmdResponse)));
        }));
}

}  // namespace mongo

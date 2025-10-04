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

#include "mongo/db/global_catalog/ddl/drop_indexes_coordinator.h"

#include "mongo/db/global_catalog/ddl/sharding_ddl_util_detail.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

DropIndexesCoordinator::DropIndexesCoordinator(ShardingDDLCoordinatorService* service,
                                               const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(service, "DropIndexesCoordinator", initialState),
      _request(_doc.getDropIndexesRequest()) {}

void DropIndexesCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = DropIndexesCoordinatorDocument::parse(
        doc, IDLParserContext("DropIndexesCoordinatorDocument"));

    const auto& selfReq = _request;
    const auto& otherReq = otherDoc.getDropIndexesRequest();

    bool areEqual =
        SimpleBSONObjComparator::kInstance.evaluate(selfReq.toBSON() == otherReq.toBSON());

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another dropIndexes for namespace "
                          << originalNss().toStringForErrorMsg()
                          << " is being executed with different parameters",
            areEqual);
}

void DropIndexesCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
};

void DropIndexesCoordinator::_dropIndexes(OperationContext* opCtx,
                                          std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) {
    auto dropIndexesRequest = _request;
    auto targetNss = nss();

    if (auto timeseriesOptions = timeseries::getTimeseriesOptions(opCtx, originalNss(), true)) {
        dropIndexesRequest.setIsTimeseriesNamespace(true);

        if (!targetNss.isTimeseriesBucketsCollection()) {
            targetNss = targetNss.makeTimeseriesBucketsNamespace();
        }
    }

    sharding::router::CollectionRouter router(opCtx->getServiceContext(), targetNss);
    router.route(
        opCtx,
        "DropIndexesCoordinator::_dropIndexesPhase",
        [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
            const auto chunkManager = cri.getChunkManager();
            std::map<ShardId, ShardVersion> shardIdsToShardVersions;

            if (chunkManager.hasRoutingTable()) {
                std::set<ShardId> shardIds;
                chunkManager.getAllShardIds(&shardIds);
                for (const auto& shardId : shardIds) {
                    shardIdsToShardVersions[shardId] =
                        ShardVersionFactory::make(chunkManager, shardId);
                }
            } else {
                shardIdsToShardVersions[ShardingState::get(opCtx)->shardId()] =
                    ShardVersion::UNSHARDED();
            }

            const auto session = getNewSession(opCtx);

            ShardsvrDropIndexesParticipant dropIndexesParticipantRequest(targetNss);
            dropIndexesParticipantRequest.setDropIndexesRequest(dropIndexesRequest);

            // TODO SERVER-107766 Remove once it is completed
            dropIndexesParticipantRequest.setRawData(isRawDataOperation(opCtx));

            generic_argument_util::setMajorityWriteConcern(dropIndexesParticipantRequest);
            generic_argument_util::setOperationSessionInfo(dropIndexesParticipantRequest, session);

            auto opts =
                std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrDropIndexesParticipant>>(
                    **executor, token, dropIndexesParticipantRequest);

            auto responses = sharding_ddl_util::sendAuthenticatedCommandToShards(
                opCtx,
                opts,
                shardIdsToShardVersions,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                false);

            BSONObjBuilder result;
            std::string errmsg;

            auto ok = appendRawResponses(opCtx, &errmsg, &result, responses).responseOK;

            if (ok && !cri.isSharded()) {
                CommandHelpers::filterCommandReplyForPassthrough(
                    responses[0].swResponse.getValue().data, &result);
            }

            CommandHelpers::appendSimpleCommandStatus(result, ok, errmsg);

            DropIndexesCoordinatorDocument newDoc = _getDoc();
            newDoc.setResult(result.obj());
            _updateStateDocument(opCtx, std::move(newDoc));

            for (const auto& cmdResponse : responses) {
                uassertStatusOK(cmdResponse.swResponse);
            }
        });
}

ExecutorFuture<void> DropIndexesCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, status, anchor = shared_from_this(), executor] {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            // Ensure migrations are resumed before terminating the coordinator.
            const auto session = getNewSession(opCtx);
            sharding_ddl_util::resumeMigrations(opCtx, nss(), boost::none /* uuid */, session);

            return Status::OK();
        });
}

ExecutorFuture<void> mongo::DropIndexesCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kFreezeMigrations,
            [this, anchor = shared_from_this(), executor, token](OperationContext* opCtx) {
                const auto session = getNewSession(opCtx);
                sharding_ddl_util::stopMigrations(opCtx, nss(), boost::none, session);
            }))
        .then(_buildPhaseHandler(
            Phase::kDropIndexes,
            [this, anchor = shared_from_this(), executor, token](OperationContext* opCtx) {
                _dropIndexes(opCtx, executor, token);
            }))
        .then(_buildPhaseHandler(
            Phase::kResumeMigrations,
            [this, anchor = shared_from_this(), executor, token](OperationContext* opCtx) {
                const auto session = getNewSession(opCtx);
                sharding_ddl_util::resumeMigrations(opCtx, nss(), boost::none, session);
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (_doc.getPhase() < Phase::kFreezeMigrations) {
                return status;
            }

            if (!_mustAlwaysMakeProgress() && !_isRetriableErrorForDDLCoordinator(status)) {
                const auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                triggerCleanup(opCtx, status);
            }
            return status;
        });
}

}  // namespace mongo

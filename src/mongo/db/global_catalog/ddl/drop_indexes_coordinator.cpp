// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/drop_indexes_coordinator.h"

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/timeseries/catalog_helper.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

DropIndexesCoordinator::DropIndexesCoordinator(ShardingCoordinatorService* service,
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

    sharding::router::CollectionRouter router(opCtx, targetNss);
    router.routeWithRoutingContext(
        "DropIndexesCoordinator::_dropIndexesPhase",
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            auto opts = [&] {
                ShardsvrDropIndexesParticipant dropIndexesParticipantRequest(targetNss);
                dropIndexesParticipantRequest.setDropIndexesRequest(dropIndexesRequest);

                // TODO SERVER-107766 Remove once it is completed
                dropIndexesParticipantRequest.setRawData(isRawDataOperation(opCtx));

                generic_argument_util::setMajorityWriteConcern(dropIndexesParticipantRequest);
                generic_argument_util::setOperationSessionInfo(dropIndexesParticipantRequest,
                                                               getNewSession(opCtx));

                return std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrDropIndexesParticipant>>(
                    **executor, token, std::move(dropIndexesParticipantRequest));
            }();

            const auto responses =
                sharding_ddl_util::sendAuthenticatedVersionedCommandTargetedByRoutingTable(
                    opCtx,
                    opts,
                    routingCtx,
                    targetNss,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    false);

            BSONObjBuilder result;
            std::string errmsg;

            auto ok = appendRawResponses(opCtx, &errmsg, &result, responses).responseOK;

            if (ok && !routingCtx.getCollectionRoutingInfo(targetNss).isSharded()) {
                CommandHelpers::filterCommandReplyForPassthrough(
                    responses[0].swResponse.getValue().data, &result);
            }

            CommandHelpers::appendSimpleCommandStatus(result, ok, errmsg);

            DropIndexesCoordinatorDocument newDoc = _copyDoc();
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
            sharding_ddl_util::resumeMigrations(
                opCtx,
                nss(),
                boost::none /* uuid */,
                [&] { return getNewSession(opCtx); },
                _doc.getAuthoritativeMetadataAccessLevel());

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
                sharding_ddl_util::stopMigrations(
                    opCtx,
                    nss(),
                    boost::none,
                    [&] { return getNewSession(opCtx); },
                    _doc.getAuthoritativeMetadataAccessLevel());
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
                sharding_ddl_util::resumeMigrations(
                    opCtx,
                    nss(),
                    boost::none,
                    [&] { return getNewSession(opCtx); },
                    _doc.getAuthoritativeMetadataAccessLevel());
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

bool DropIndexesCoordinator::isInCriticalSection(Phase phase) const {
    // No critical section is taken
    return false;
}

}  // namespace mongo

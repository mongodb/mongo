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


#include "mongo/db/s/collmod_coordinator.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/s/participant_block_gen.h"
#include "mongo/db/s/sharded_collmod_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

MONGO_FAIL_POINT_DEFINE(collModBeforeConfigServerUpdate);

namespace {

bool isShardedColl(OperationContext* opCtx, const NamespaceString& nss) {
    try {
        auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
        return true;
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is not sharded or doesn't exist.
        return false;
    }
}

bool hasTimeSeriesGranularityUpdate(const CollModRequest& request) {
    return request.getTimeseries() && request.getTimeseries()->getGranularity();
}

}  // namespace

CollModCoordinator::CollModCoordinator(ShardingDDLCoordinatorService* service,
                                       const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(service, "CollModCoordinator", initialState),
      _request{_doc.getCollModRequest()} {}

void CollModCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc =
        CollModCoordinatorDocument::parse(IDLParserContext("CollModCoordinatorDocument"), doc);

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getCollModRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another collMod for namespace " << originalNss()
                          << " is being executed with different parameters: " << selfReq,
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

void CollModCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
};

// TODO SERVER-68008 Remove once 7.0 becomes last LTS
bool CollModCoordinator::_isPre61Compatible() const {
    return operationType() == DDLCoordinatorTypeEnum::kCollModPre61Compatible;
}

void CollModCoordinator::_performNoopRetryableWriteOnParticipants(
    OperationContext* opCtx, const std::shared_ptr<executor::TaskExecutor>& executor) {
    auto shardsAndConfigsvr = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto participants = shardRegistry->getAllShardIds(opCtx);
        participants.emplace_back(shardRegistry->getConfigShard()->getId());
        return participants;
    }();

    _updateSession(opCtx);
    sharding_ddl_util::performNoopRetryableWriteOnShards(
        opCtx, shardsAndConfigsvr, getCurrentSession(), executor);
}

void CollModCoordinator::_saveCollectionInfoOnCoordinatorIfNecessary(OperationContext* opCtx) {
    if (!_collInfo) {
        CollectionInfo info;
        info.timeSeriesOptions = timeseries::getTimeseriesOptions(opCtx, originalNss(), true);
        info.nsForTargeting =
            info.timeSeriesOptions ? originalNss().makeTimeseriesBucketsNamespace() : originalNss();
        info.isSharded = isShardedColl(opCtx, info.nsForTargeting);
        _collInfo = std::move(info);
    }
}

void CollModCoordinator::_saveShardingInfoOnCoordinatorIfNecessary(OperationContext* opCtx) {
    tassert(
        6522700, "Sharding information must be gathered after collection information", _collInfo);
    if (!_shardingInfo && _collInfo->isSharded) {
        ShardingInfo info;
        const auto chunkManager =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
                opCtx, _collInfo->nsForTargeting));

        info.primaryShard = chunkManager.dbPrimary();
        std::set<ShardId> shardIdsSet;
        chunkManager.getAllShardIds(&shardIdsSet);
        std::vector<ShardId> shardIdsVec{shardIdsSet.begin(), shardIdsSet.end()};
        info.shardsOwningChunks = std::move(shardIdsVec);
        _shardingInfo = std::move(info);
    }
}

ExecutorFuture<void> CollModCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor = executor, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            if (_doc.getPhase() > Phase::kUnset) {
                _performNoopRetryableWriteOnParticipants(opCtx, **executor);
            }

            {
                AutoGetCollection coll{
                    opCtx, nss(), MODE_IS, AutoGetCollectionViewMode::kViewsPermitted};
                checkCollectionUUIDMismatch(opCtx, nss(), *coll, _request.getCollectionUUID());
            }

            _saveCollectionInfoOnCoordinatorIfNecessary(opCtx);

            auto isGranularityUpdate = hasTimeSeriesGranularityUpdate(_request);
            uassert(6201808,
                    "Cannot use time-series options for a non-timeseries collection",
                    _collInfo->timeSeriesOptions || !isGranularityUpdate);
            if (isGranularityUpdate) {
                uassert(ErrorCodes::InvalidOptions,
                        "Invalid transition for timeseries.granularity. Can only transition "
                        "from 'seconds' to 'minutes' or 'minutes' to 'hours'.",
                        timeseries::isValidTimeseriesGranularityTransition(
                            _collInfo->timeSeriesOptions->getGranularity(),
                            *_request.getTimeseries()->getGranularity()));
            }
        })
        .then([this, executor = executor, anchor = shared_from_this()] {
            if (_isPre61Compatible()) {
                return;
            }
            _executePhase(
                Phase::kFreezeMigrations, [this, executor = executor, anchor = shared_from_this()] {
                    auto opCtxHolder = cc().makeOperationContext();
                    auto* opCtx = opCtxHolder.get();
                    getForwardableOpMetadata().setOn(opCtx);

                    _saveCollectionInfoOnCoordinatorIfNecessary(opCtx);

                    if (_collInfo->isSharded) {
                        _doc.setCollUUID(
                            sharding_ddl_util::getCollectionUUID(opCtx, _collInfo->nsForTargeting));
                        sharding_ddl_util::stopMigrations(
                            opCtx, _collInfo->nsForTargeting, _doc.getCollUUID());
                    }
                });
        })
        .then(_executePhase(
            Phase::kBlockShards,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                _updateSession(opCtx);

                _saveCollectionInfoOnCoordinatorIfNecessary(opCtx);

                if (_isPre61Compatible() && _collInfo->isSharded) {
                    _doc.setCollUUID(sharding_ddl_util::getCollectionUUID(
                        opCtx, originalNss(), true /* allowViews */));
                    sharding_ddl_util::stopMigrations(opCtx, originalNss(), _doc.getCollUUID());
                }

                _saveShardingInfoOnCoordinatorIfNecessary(opCtx);

                if (_collInfo->isSharded && hasTimeSeriesGranularityUpdate(_request)) {
                    ShardsvrParticipantBlock blockCRUDOperationsRequest(_collInfo->nsForTargeting);
                    const auto cmdObj = CommandHelpers::appendMajorityWriteConcern(
                        blockCRUDOperationsRequest.toBSON({}));
                    sharding_ddl_util::sendAuthenticatedCommandToShards(
                        opCtx, nss().db(), cmdObj, _shardingInfo->shardsOwningChunks, **executor);
                }
            }))
        .then(_executePhase(
            Phase::kUpdateConfig,
            [this, executor = executor, anchor = shared_from_this()] {
                collModBeforeConfigServerUpdate.pauseWhileSet();

                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                _updateSession(opCtx);

                _saveCollectionInfoOnCoordinatorIfNecessary(opCtx);
                _saveShardingInfoOnCoordinatorIfNecessary(opCtx);

                if (_collInfo->isSharded && _collInfo->timeSeriesOptions &&
                    hasTimeSeriesGranularityUpdate(_request)) {
                    ConfigsvrCollMod request(_collInfo->nsForTargeting, _request);
                    const auto cmdObj =
                        CommandHelpers::appendMajorityWriteConcern(request.toBSON({}));

                    const auto& configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
                    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(
                        configShard->runCommand(opCtx,
                                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                nss().db().toString(),
                                                cmdObj,
                                                Shard::RetryPolicy::kIdempotent)));
                }
            }))
        .then(_executePhase(
            Phase::kUpdateShards,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                _updateSession(opCtx);

                _saveCollectionInfoOnCoordinatorIfNecessary(opCtx);
                _saveShardingInfoOnCoordinatorIfNecessary(opCtx);

                if (_collInfo->isSharded) {
                    ShardsvrCollModParticipant request(originalNss(), _request);
                    bool needsUnblock =
                        _collInfo->timeSeriesOptions && hasTimeSeriesGranularityUpdate(_request);
                    request.setNeedsUnblock(needsUnblock);

                    std::vector<AsyncRequestsSender::Response> responses;
                    auto shardsOwningChunks = _shardingInfo->shardsOwningChunks;
                    auto primaryShardOwningChunk = std::find(shardsOwningChunks.begin(),
                                                             shardsOwningChunks.end(),
                                                             _shardingInfo->primaryShard);
                    // A view definition will only be present on the primary shard. So we pass an
                    // addition 'performViewChange' flag only to the primary shard.
                    if (primaryShardOwningChunk != shardsOwningChunks.end()) {
                        request.setPerformViewChange(true);
                        const auto& primaryResponse =
                            sharding_ddl_util::sendAuthenticatedCommandToShards(
                                opCtx,
                                nss().db(),
                                CommandHelpers::appendMajorityWriteConcern(request.toBSON({})),
                                {_shardingInfo->primaryShard},
                                **executor);
                        responses.insert(
                            responses.end(), primaryResponse.begin(), primaryResponse.end());
                        shardsOwningChunks.erase(primaryShardOwningChunk);
                    }

                    request.setPerformViewChange(false);
                    const auto& secondaryResponses =
                        sharding_ddl_util::sendAuthenticatedCommandToShards(
                            opCtx,
                            nss().db(),
                            CommandHelpers::appendMajorityWriteConcern(request.toBSON({})),
                            shardsOwningChunks,
                            **executor);
                    responses.insert(
                        responses.end(), secondaryResponses.begin(), secondaryResponses.end());

                    BSONObjBuilder builder;
                    std::string errmsg;
                    auto ok = appendRawResponses(opCtx, &errmsg, &builder, responses).responseOK;
                    if (!errmsg.empty()) {
                        CommandHelpers::appendSimpleCommandStatus(builder, ok, errmsg);
                    }
                    _result = builder.obj();
                    sharding_ddl_util::resumeMigrations(
                        opCtx,
                        _isPre61Compatible() ? originalNss() : _collInfo->nsForTargeting,
                        _doc.getCollUUID());
                } else {
                    CollMod cmd(originalNss());
                    cmd.setCollModRequest(_request);
                    BSONObjBuilder collModResBuilder;
                    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
                        opCtx, originalNss(), cmd, true, &collModResBuilder));
                    auto collModRes = collModResBuilder.obj();

                    const auto dbInfo = uassertStatusOK(
                        Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss().db()));
                    const auto shard = uassertStatusOK(
                        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbInfo->getPrimary()));
                    BSONObjBuilder builder;
                    builder.appendElements(collModRes);
                    BSONObjBuilder subBuilder(builder.subobjStart("raw"));
                    subBuilder.append(shard->getConnString().toString(), collModRes);
                    subBuilder.doneFast();
                    _result = builder.obj();
                }
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5757002,
                            "Error running collMod",
                            "namespace"_attr = nss(),
                            "error"_attr = redact(status));
                // If we have the collection UUID set, this error happened in a sharded collection,
                // we should restore the migrations.
                if (_doc.getCollUUID()) {
                    auto opCtxHolder = cc().makeOperationContext();
                    auto* opCtx = opCtxHolder.get();
                    getForwardableOpMetadata().setOn(opCtx);

                    sharding_ddl_util::resumeMigrations(
                        opCtx,
                        _isPre61Compatible() ? originalNss() : _collInfo->nsForTargeting,
                        _doc.getCollUUID());
                }
            }
            return status;
        });
}

}  // namespace mongo

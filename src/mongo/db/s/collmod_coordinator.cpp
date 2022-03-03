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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/collmod_coordinator.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/s/sharded_collmod_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {

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
    : ShardingDDLCoordinator(service, initialState) {
    _initialState = initialState.getOwned();
    _doc = CollModCoordinatorDocument::parse(IDLParserErrorContext("CollModCoordinatorDocument"),
                                             _initialState);
}

void CollModCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc =
        CollModCoordinatorDocument::parse(IDLParserErrorContext("CollModCoordinatorDocument"), doc);

    const auto& selfReq = _doc.getCollModRequest().toBSON();
    const auto& otherReq = otherDoc.getCollModRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another collMod for namespace " << nss()
                          << " is being executed with different parameters: " << selfReq,
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

boost::optional<BSONObj> CollModCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder cmdBob;
    if (const auto& optComment = getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    cmdBob.appendElements(_doc.getCollModRequest().toBSON());
    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "CollModCoordinator");
    bob.append("op", "command");
    bob.append("ns", nss().toString());
    bob.append("command", cmdBob.obj());
    bob.append("currentPhase", _doc.getPhase());
    bob.append("active", true);
    return bob.obj();
}

void CollModCoordinator::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(6069401,
                2,
                "CollMod coordinator phase transition",
                "namespace"_attr = nss(),
                "newPhase"_attr = CollModCoordinatorPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = CollModCoordinatorPhase_serializer(_doc.getPhase()));

    if (_doc.getPhase() == Phase::kUnset) {
        _doc = _insertStateDocument(std::move(newDoc));
        return;
    }
    _doc = _updateStateDocument(cc().makeOperationContext().get(), std::move(newDoc));
}

void CollModCoordinator::_performNoopRetryableWriteOnParticipants(
    OperationContext* opCtx, const std::shared_ptr<executor::TaskExecutor>& executor) {
    auto shardsAndConfigsvr = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto participants = shardRegistry->getAllShardIds(opCtx);
        participants.emplace_back(shardRegistry->getConfigShard()->getId());
        return participants;
    }();

    _doc = _updateSession(opCtx, _doc);
    sharding_ddl_util::performNoopRetryableWriteOnShards(
        opCtx, shardsAndConfigsvr, getCurrentSession(_doc), executor);
}

ExecutorFuture<void> CollModCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(
            Phase::kUpdateShards,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                {
                    AutoGetCollection coll{
                        opCtx, nss(), MODE_IS, AutoGetCollectionViewMode::kViewsPermitted};
                    checkCollectionUUIDMismatch(
                        opCtx, nss(), *coll, _doc.getCollModRequest().getCollectionUUID());
                }

                const auto isTimeSeries = timeseries::getTimeseriesOptions(
                    opCtx, nss(), !nss().isTimeseriesBucketsCollection());
                const auto collNss = isTimeSeries && !nss().isTimeseriesBucketsCollection()
                    ? nss().makeTimeseriesBucketsNamespace()
                    : nss();
                const auto isSharded = isShardedColl(opCtx, collNss);

                if (isSharded) {
                    // Updating granularity on sharded time-series collections is not allowed.
                    if (isTimeSeries) {
                        uassert(
                            ErrorCodes::NotImplemented,
                            str::stream()
                                << "Cannot update granularity of a sharded time-series collection.",
                            !hasTimeSeriesGranularityUpdate(_doc.getCollModRequest()));
                    }
                    _doc.setCollUUID(
                        sharding_ddl_util::getCollectionUUID(opCtx, nss(), true /* allowViews */));

                    sharding_ddl_util::stopMigrations(opCtx, nss(), _doc.getCollUUID());

                    if (!_firstExecution) {
                        _performNoopRetryableWriteOnParticipants(opCtx, **executor);
                    }

                    _doc = _updateSession(opCtx, _doc);
                    const OperationSessionInfo osi = getCurrentSession(_doc);

                    const auto chunkManager = uassertStatusOK(
                        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
                            opCtx, collNss));
                    std::unique_ptr<CollatorInterface> collator;
                    const auto expCtx =
                        make_intrusive<ExpressionContext>(opCtx, std::move(collator), collNss);
                    std::set<ShardId> participants;
                    chunkManager.getShardIdsForQuery(
                        expCtx, {} /* query */, {} /* collation */, &participants);

                    ShardsvrCollModParticipant request(nss(), _doc.getCollModRequest());
                    const auto cmdObj =
                        CommandHelpers::appendMajorityWriteConcern(request.toBSON({}));
                    const auto& responses = sharding_ddl_util::sendAuthenticatedCommandToShards(
                        opCtx,
                        nss().db(),
                        cmdObj.addFields(osi.toBSON()),
                        {std::make_move_iterator(participants.begin()),
                         std::make_move_iterator(participants.end())},
                        **executor);
                    BSONObjBuilder builder;
                    std::string errmsg;
                    auto ok = appendRawResponses(opCtx, &errmsg, &builder, responses).responseOK;
                    if (!errmsg.empty()) {
                        CommandHelpers::appendSimpleCommandStatus(builder, ok, errmsg);
                    }
                    _result = builder.obj();
                    sharding_ddl_util::resumeMigrations(opCtx, nss(), _doc.getCollUUID());
                } else {
                    CollMod cmd(nss());
                    cmd.setCollModRequest(_doc.getCollModRequest());
                    BSONObjBuilder collModResBuilder;
                    uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
                        opCtx, nss(), cmd, &collModResBuilder));
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

                    sharding_ddl_util::resumeMigrations(opCtx, nss(), _doc.getCollUUID());
                }
            }
            return status;
        });
}

}  // namespace mongo

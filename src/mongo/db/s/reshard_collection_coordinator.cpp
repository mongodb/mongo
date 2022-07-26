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


#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/s/reshard_collection_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/reshard_collection_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

void notifyChangeStreamsOnReshardCollectionComplete(OperationContext* opCtx,
                                                    const NamespaceString& collNss,
                                                    const ReshardCollectionCoordinatorDocument& doc,
                                                    const UUID& reshardUUID) {

    const std::string oMessage = str::stream()
        << "Reshard collection " << collNss << " with shard key " << doc.getKey().toString();

    BSONObjBuilder cmdBuilder;
    tassert(6590800, "Did not set old collectionUUID", doc.getOldCollectionUUID());
    tassert(6590801, "Did not set old ShardKey", doc.getOldShardKey());
    UUID collUUID = *doc.getOldCollectionUUID();
    cmdBuilder.append("reshardCollection", collNss.ns());
    reshardUUID.appendToBuilder(&cmdBuilder, "reshardUUID");
    cmdBuilder.append("shardKey", doc.getKey());
    cmdBuilder.append("oldShardKey", *doc.getOldShardKey());

    cmdBuilder.append("unique", doc.getUnique().get_value_or(false));
    if (doc.getNumInitialChunks()) {
        cmdBuilder.append("numInitialChunks", doc.getNumInitialChunks().get());
    }
    if (doc.getCollation()) {
        cmdBuilder.append("collation", doc.getCollation().get());
    }

    if (doc.getZones()) {
        BSONArrayBuilder zonesBSON(cmdBuilder.subarrayStart("zones"));
        for (const auto& zone : *doc.getZones()) {
            zonesBSON.append(zone.toBSON());
        }
        zonesBSON.doneFast();
    }

    auto const serviceContext = opCtx->getClient()->getServiceContext();

    const auto cmd = cmdBuilder.obj();

    writeConflictRetry(opCtx, "ReshardCollection", NamespaceString::kRsOplogNamespace.ns(), [&] {
        AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
        WriteUnitOfWork uow(opCtx);
        serviceContext->getOpObserver()->onInternalOpMessage(opCtx,
                                                             collNss,
                                                             collUUID,
                                                             BSON("msg" << oMessage),
                                                             cmd,
                                                             boost::none,
                                                             boost::none,
                                                             boost::none,
                                                             boost::none);
        uow.commit();
    });
}
}  // namespace

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
        .then(_executePhase(
            Phase::kReshard,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                {
                    AutoGetCollection coll{
                        opCtx, nss(), MODE_IS, AutoGetCollectionViewMode::kViewsPermitted};
                    checkCollectionUUIDMismatch(opCtx, nss(), *coll, _doc.getCollectionUUID());
                }

                const auto cmOld = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
                        opCtx, nss()));

                StateDoc newDoc(_doc);
                newDoc.setOldShardKey(cmOld.getShardKeyPattern().getKeyPattern().toBSON());
                newDoc.setOldCollectionUUID(cmOld.getUUID());
                _updateStateDocument(opCtx, std::move(newDoc));

                ConfigsvrReshardCollection configsvrReshardCollection(nss(), _doc.getKey());
                configsvrReshardCollection.setDbName(nss().db());
                configsvrReshardCollection.setUnique(_doc.getUnique());
                configsvrReshardCollection.setCollation(_doc.getCollation());
                configsvrReshardCollection.set_presetReshardedChunks(
                    _doc.get_presetReshardedChunks());
                configsvrReshardCollection.setZones(_doc.getZones());
                configsvrReshardCollection.setNumInitialChunks(_doc.getNumInitialChunks());

                const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

                const auto cmdResponse =
                    uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                        opCtx,
                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                        NamespaceString::kAdminDb.toString(),
                        CommandHelpers::appendMajorityWriteConcern(
                            configsvrReshardCollection.toBSON({}), opCtx->getWriteConcern()),
                        Shard::RetryPolicy::kIdempotent));
                uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(std::move(cmdResponse)));

                // Report command completion to the oplog.
                const auto cm = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
                        opCtx, nss()));

                if (_doc.getOldCollectionUUID() && _doc.getOldCollectionUUID() != cm.getUUID()) {
                    notifyChangeStreamsOnReshardCollectionComplete(
                        opCtx, nss(), _doc, cm.getUUID());
                }
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(6206401,
                        "Error running reshard collection",
                        "namespace"_attr = nss(),
                        "error"_attr = redact(status));
            return status;
        });
}

}  // namespace mongo

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


#include "mongo/db/s/refine_collection_shard_key_coordinator.h"

#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

void notifyChangeStreamsOnRefineCollectionShardKeyComplete(OperationContext* opCtx,
                                                           const NamespaceString& collNss,
                                                           const KeyPattern& shardKey,
                                                           const KeyPattern& oldShardKey,
                                                           const UUID& collUUID) {

    const std::string oMessage = str::stream()
        << "Refine shard key for collection " << collNss << " with " << shardKey.toString();

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("refineCollectionShardKey", collNss.ns());
    cmdBuilder.append("shardKey", shardKey.toBSON());
    cmdBuilder.append("oldShardKey", oldShardKey.toBSON());

    auto const serviceContext = opCtx->getClient()->getServiceContext();

    writeConflictRetry(
        opCtx, "RefineCollectionShardKey", NamespaceString::kRsOplogNamespace.ns(), [&] {
            AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
            WriteUnitOfWork uow(opCtx);
            serviceContext->getOpObserver()->onInternalOpMessage(opCtx,
                                                                 collNss,
                                                                 collUUID,
                                                                 BSON("msg" << oMessage),
                                                                 cmdBuilder.obj(),
                                                                 boost::none,
                                                                 boost::none,
                                                                 boost::none,
                                                                 boost::none);
            uow.commit();
        });
}
}  // namespace

RefineCollectionShardKeyCoordinator::RefineCollectionShardKeyCoordinator(
    ShardingDDLCoordinatorService* service, const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(
          service, "RefineCollectionShardKeyCoordinator", initialState),
      _request(_doc.getRefineCollectionShardKeyRequest()),
      _newShardKey(_doc.getNewShardKey()) {}

void RefineCollectionShardKeyCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two refine collections on the same namespace, then the arguments must be the same.
    const auto otherDoc = RefineCollectionShardKeyCoordinatorDocument::parse(
        IDLParserContext("RefineCollectionShardKeyCoordinatorDocument"), doc);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another refine collection with different arguments is already running for the same "
            "namespace",
            SimpleBSONObjComparator::kInstance.evaluate(
                _request.toBSON() == otherDoc.getRefineCollectionShardKeyRequest().toBSON()));
}

void RefineCollectionShardKeyCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

ExecutorFuture<void> RefineCollectionShardKeyCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(
            Phase::kRefineCollectionShardKey,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                {
                    AutoGetCollection coll{
                        opCtx, nss(), MODE_IS, AutoGetCollectionViewMode::kViewsPermitted};
                    checkCollectionUUIDMismatch(opCtx, nss(), *coll, _request.getCollectionUUID());
                }

                shardkeyutil::validateShardKeyIsNotEncrypted(
                    opCtx, nss(), ShardKeyPattern(_newShardKey.toBSON()));

                const auto cm = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
                        opCtx, nss()));

                _oldShardKey = cm.getShardKeyPattern().getKeyPattern();
                _collectionUUID = cm.getUUID();

                ConfigsvrRefineCollectionShardKey configsvrRefineCollShardKey(
                    nss(), _newShardKey.toBSON(), cm.getVersion().epoch());
                configsvrRefineCollShardKey.setDbName(nss().db().toString());
                configsvrRefineCollShardKey.setEnforceUniquenessCheck(
                    _request.getEnforceUniquenessCheck());
                auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

                sharding_ddl_util::stopMigrations(opCtx, nss(), boost::none);

                const auto cmdResponse = uassertStatusOK(configShard->runCommand(
                    opCtx,
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    NamespaceString::kAdminDb.toString(),
                    CommandHelpers::appendMajorityWriteConcern(
                        configsvrRefineCollShardKey.toBSON({}), opCtx->getWriteConcern()),
                    Shard::RetryPolicy::kIdempotent));

                uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(std::move(cmdResponse)));
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5277700,
                        "Error running refine collection shard key",
                        "namespace"_attr = nss(),
                        "error"_attr = redact(status));

            return status;
        })
        .onCompletion([this, anchor = shared_from_this()](const Status& status) {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            if (status.isOK()) {
                tassert(6295600, "Failed to set collection uuid", _collectionUUID);
                notifyChangeStreamsOnRefineCollectionShardKeyComplete(
                    opCtx, nss(), _newShardKey, _oldShardKey, *_collectionUUID);
            }

            sharding_ddl_util::resumeMigrations(opCtx, nss(), boost::none);

            return status;
        });
}

}  // namespace mongo

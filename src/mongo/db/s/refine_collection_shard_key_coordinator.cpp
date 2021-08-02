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

#include "mongo/db/s/refine_collection_shard_key_coordinator.h"

#include "mongo/db/commands.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/refine_collection_shard_key_gen.h"

namespace mongo {

RefineCollectionShardKeyCoordinator::RefineCollectionShardKeyCoordinator(
    ShardingDDLCoordinatorService* service, const BSONObj& initialState)
    : ShardingDDLCoordinator(service, initialState),
      _doc(RefineCollectionShardKeyCoordinatorDocument::parse(
          IDLParserErrorContext("RefineCollectionShardKeyCoordinatorDocument"), initialState)),
      _newShardKey(_doc.getNewShardKey()) {}


void RefineCollectionShardKeyCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two shard collections on the same namespace, then the arguments must be the same.
    const auto otherDoc = RefineCollectionShardKeyCoordinatorDocument::parse(
        IDLParserErrorContext("RefineCollectionShardKeyCoordinatorDocument"), doc);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another create collection with different arguments is already running for the same "
            "namespace",
            SimpleBSONObjComparator::kInstance.evaluate(
                _doc.getRefineCollectionShardKeyRequest().toBSON() ==
                otherDoc.getRefineCollectionShardKeyRequest().toBSON()));
}

boost::optional<BSONObj> RefineCollectionShardKeyCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    BSONObjBuilder cmdBob;
    if (const auto& optComment = getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    cmdBob.appendElements(_doc.getRefineCollectionShardKeyRequest().toBSON());

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "RefineCollectionShardKeyCoordinator");
    bob.append("op", "command");
    bob.append("ns", nss().toString());
    bob.append("command", cmdBob.obj());
    bob.append("active", true);
    return bob.obj();
}

ExecutorFuture<void> RefineCollectionShardKeyCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            const auto cm = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
                    opCtx, nss()));
            ConfigsvrRefineCollectionShardKey configsvrRefineCollShardKey(
                nss(), _newShardKey.toBSON(), cm.getVersion().epoch());
            configsvrRefineCollShardKey.setDbName(nss().db().toString());
            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            try {
                auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                    opCtx,
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    NamespaceString::kAdminDb.toString(),
                    CommandHelpers::appendMajorityWriteConcern(
                        configsvrRefineCollShardKey.toBSON({}), opCtx->getWriteConcern()),
                    Shard::RetryPolicy::kIdempotent));

                uassertStatusOK(cmdResponse.commandStatus);
                uassertStatusOK(cmdResponse.writeConcernStatus);
            } catch (const DBException&) {
                _completeOnError = true;
                throw;
            }
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5277700,
                        "Error running refine collection shard key",
                        "namespace"_attr = nss(),
                        "error"_attr = redact(status));
            return status;
        });
}

}  // namespace mongo

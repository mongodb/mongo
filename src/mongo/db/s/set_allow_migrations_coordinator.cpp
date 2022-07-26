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


#include "mongo/db/s/set_allow_migrations_coordinator.h"

#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

bool isCollectionSharded(OperationContext* opCtx, const NamespaceString& nss) {
    try {
        Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
        return true;
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is unsharded or doesn't exist
        return false;
    }
}

void SetAllowMigrationsCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two set allow migrations on the same namespace, then the arguments must be the
    // same.
    const auto otherDoc = SetAllowMigrationsCoordinatorDocument::parse(
        IDLParserContext("SetAllowMigrationsCoordinatorDocument"), doc);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another set allow migrations with different arguments is already running for the same "
            "namespace",
            SimpleBSONObjComparator::kInstance.evaluate(
                _doc.getSetAllowMigrationsRequest().toBSON() ==
                otherDoc.getSetAllowMigrationsRequest().toBSON()));
}

void SetAllowMigrationsCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    stdx::lock_guard lk{_docMutex};
    cmdInfoBuilder->appendElements(_doc.getSetAllowMigrationsRequest().toBSON());
}

ExecutorFuture<void> SetAllowMigrationsCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            uassert(ErrorCodes::NamespaceNotSharded,
                    "Collection must be sharded so migrations can be blocked",
                    isCollectionSharded(opCtx, nss()));

            const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            BatchedCommandRequest updateRequest([&]() {
                write_ops::UpdateCommandRequest updateOp(CollectionType::ConfigNS);
                updateOp.setUpdates({[&] {
                    write_ops::UpdateOpEntry entry;
                    entry.setQ(BSON(CollectionType::kNssFieldName << nss().ns()));
                    if (_allowMigrations) {
                        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON(
                            "$unset" << BSON(CollectionType::kPermitMigrationsFieldName << true))));
                    } else {
                        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON(
                            "$set" << BSON(CollectionType::kPermitMigrationsFieldName << false))));
                    }
                    entry.setMulti(false);
                    return entry;
                }()});
                return updateOp;
            }());

            updateRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

            auto response = configShard->runBatchWriteCommand(opCtx,
                                                              Shard::kDefaultConfigCommandTimeout,
                                                              updateRequest,
                                                              Shard::RetryPolicy::kIdempotent);

            uassertStatusOK(response.toStatus());
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5622700,
                        "Error running set allow migrations",
                        "namespace"_attr = nss(),
                        "error"_attr = redact(status));
            return status;
        });
}

}  // namespace mongo

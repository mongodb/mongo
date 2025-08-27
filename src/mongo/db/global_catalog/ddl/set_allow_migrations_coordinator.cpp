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


#include "mongo/db/global_catalog/ddl/set_allow_migrations_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"

#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

bool isCollectionSharded(OperationContext* opCtx, const NamespaceString& nss) {
    try {
        const auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
        if (coll.getUnsplittable().value_or(false)) {
            return false;
        }
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
        doc, IDLParserContext("SetAllowMigrationsCoordinatorDocument"));

    stdx::lock_guard lk{_docMutex};
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
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtxHolder = this->makeOperationContext();
        auto* opCtx = opCtxHolder.get();

        uassert(ErrorCodes::NamespaceNotSharded,
                "Collection must be sharded so migrations can be blocked",
                isCollectionSharded(opCtx, nss()));

        const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        BatchedCommandRequest updateRequest([&]() {
            write_ops::UpdateCommandRequest updateOp(CollectionType::ConfigNS);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                                    nss(), SerializationContext::stateDefault())));
                if (_allowMigrations) {
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON(
                        "$unset" << BSON(CollectionType::kPermitMigrationsFieldName << true))));
                } else {
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                        BSON("$set" << BSON(CollectionType::kPermitMigrationsFieldName << false))));
                }
                entry.setMulti(false);
                return entry;
            }()});
            return updateOp;
        }());

        auto response =
            configShard->runBatchWriteCommand(opCtx,
                                              Milliseconds(defaultConfigCommandTimeoutMS.load()),
                                              updateRequest,
                                              defaultMajorityWriteConcernDoNotUse(),
                                              Shard::RetryPolicy::kIdempotent);

        uassertStatusOK(response.toStatus());

        ShardingLogging::get(opCtx)->logChange(
            opCtx, "setPermitMigrations", nss(), BSON("permitMigrations" << _allowMigrations));
    });
}

}  // namespace mongo

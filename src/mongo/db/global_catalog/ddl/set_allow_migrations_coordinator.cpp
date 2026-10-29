// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/global_catalog/ddl/set_allow_migrations_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
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

    std::lock_guard lk{_docMutex};
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another set allow migrations with different arguments is already running for the same "
            "namespace",
            SimpleBSONObjComparator::kInstance.evaluate(
                _doc.getSetAllowMigrationsRequest().toBSON() ==
                otherDoc.getSetAllowMigrationsRequest().toBSON()));
}

void SetAllowMigrationsCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    std::lock_guard lk{_docMutex};
    cmdInfoBuilder->appendElements(_doc.getSetAllowMigrationsRequest().toBSON());
}

ExecutorFuture<void> SetAllowMigrationsCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtxHolder = this->makeOperationContext(/*deprioritizable=*/true);
        auto* opCtx = opCtxHolder.get();

        uassert(ErrorCodes::NamespaceNotSharded,
                "Collection must be sharded so migrations can be blocked",
                isCollectionSharded(opCtx, nss()));

        const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        BatchedCommandRequest updateRequest([&]() {
            write_ops::UpdateCommandRequest updateOp(
                NamespaceString::kConfigsvrCollectionsNamespace);
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

bool SetAllowMigrationsCoordinator::isInCriticalSection(Phase phase) const {
    // No critical section is taken
    return false;
}
}  // namespace mongo

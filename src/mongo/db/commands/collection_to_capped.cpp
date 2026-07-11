// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/collection_crud/capped_utils.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>

namespace mongo {
namespace {

class CmdCloneCollectionAsCapped : public BasicCommand {
public:
    CmdCloneCollectionAsCapped() : BasicCommand("cloneCollectionAsCapped") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "{ cloneCollectionAsCapped:<fromName>, toCollection:<toName>, size:<sizeInBytes> }";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::find)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        const auto nssElt = cmdObj["toCollection"];
        uassert(ErrorCodes::TypeMismatch,
                "'toCollection' must be of type String",
                nssElt.type() == BSONType::string);
        const NamespaceString nss(
            NamespaceStringUtil::deserialize(dbName, nssElt.valueStringData()));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target namespace: " << nss.toStringForErrorMsg(),
                nss.isValid());

        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(nss),
                {ActionType::insert, ActionType::createIndex, ActionType::convertToCapped})) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto fromElt = cmdObj["cloneCollectionAsCapped"];
        const auto toElt = cmdObj["toCollection"];

        uassert(ErrorCodes::TypeMismatch,
                "'cloneCollectionAsCapped' must be of type String",
                fromElt.type() == BSONType::string);
        uassert(ErrorCodes::TypeMismatch,
                "'toCollection' must be of type String",
                toElt.type() == BSONType::string);

        const std::string_view from(fromElt.valueStringData());
        const std::string_view to(toElt.valueStringData());

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid source collection name: " << from,
                NamespaceString::validCollectionName(from));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target collection name: " << to,
                NamespaceString::validCollectionName(to));

        auto size = cmdObj.getField("size").safeNumberLong();
        bool temp = cmdObj.getField("temp").trueValue();

        uassert(ErrorCodes::InvalidOptions,
                "Capped collection size must be greater than zero",
                size > 0);

        NamespaceString fromNs(NamespaceStringUtil::deserialize(dbName, from));
        NamespaceString toNs(NamespaceStringUtil::deserialize(dbName, to));

        ReplicaSetWriteBlockState::get(opCtx)->checkReplicaSetWritesAllowed(
            opCtx, toNs, ReplicaSetWriteBlockRejectedWriteOp::kInsert);

        ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
            opCtx, std::vector<NamespaceString>{fromNs, toNs});
        // Similarly to other DDL commands, e.g., CmdCreate, the collection creation for the
        // specified toNs namespace is legitimate since it is part of the command intended
        // functionality.
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE allowCreate(opCtx, toNs);

        CollectionAcquisitionRequests acquisitionRequests = {
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, fromNs, AcquisitionPrerequisites::OperationType::kWrite),
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, toNs, AcquisitionPrerequisites::OperationType::kWrite)};
        auto acquisitions =
            makeAcquisitionMap(acquireCollections(opCtx, acquisitionRequests, LockMode::MODE_X));
        tassert(
            10769700, "Expected acquisition map to contain fromNs", acquisitions.contains(fromNs));
        tassert(10769701, "Expected acquisition map to contain toNs", acquisitions.contains(toNs));

        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, toNs)) {
            uasserted(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while cloning collection " << from << " to "
                                    << to << " (as capped)");
        }

        auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, dbName);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName.toStringForErrorMsg() << " not found",
                db);


        CollectionAcquisition fromColl = acquisitions.at(fromNs);
        CollectionAcquisition toColl = acquisitions.at(toNs);
        cloneCollectionAsCapped(opCtx, db, fromColl, toColl, size, temp);
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdCloneCollectionAsCapped).forShard();

/**
 * Converts the given collection to a capped collection w/ the specified size. This command is not
 * highly used, and is not currently supported with sharded environments.
 */
class CmdConvertToCapped : public BasicCommand {
public:
    CmdConvertToCapped() : BasicCommand("convertToCapped") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return "{ convertToCapped:<fromCollectionName>, size:<sizeInBytes> }";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::convertToCapped)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

        ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
            opCtx, std::vector<NamespaceString>{nss});

        auto size = cmdObj.getField("size").safeNumberLong();

        uassert(ErrorCodes::InvalidOptions,
                "Capped collection size must be greater than zero",
                size > 0);

        convertToCapped(opCtx, nss, size, false /*fromMigrate*/);
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdConvertToCapped).forShard();

}  // namespace
}  // namespace mongo

/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
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
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <string>

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

        const StringData from(fromElt.valueStringData());
        const StringData to(toElt.valueStringData());

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

        ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
            opCtx, std::vector<NamespaceString>{fromNs, toNs});

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

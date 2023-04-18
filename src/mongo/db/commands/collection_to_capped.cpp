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

#include "mongo/platform/basic.h"


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"

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
                nssElt.type() == BSONType::String);
        const NamespaceString nss(
            NamespaceStringUtil::parseNamespaceFromRequest(dbName, nssElt.valueStringData()));
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
                fromElt.type() == BSONType::String);
        uassert(ErrorCodes::TypeMismatch,
                "'toCollection' must be of type String",
                toElt.type() == BSONType::String);

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

        uassert(ErrorCodes::InvalidOptions, "invalid command spec", size != 0);

        NamespaceString fromNs(NamespaceStringUtil::parseNamespaceFromRequest(dbName, from));
        NamespaceString toNs(NamespaceStringUtil::parseNamespaceFromRequest(dbName, to));

        AutoGetCollection autoColl(opCtx, fromNs, MODE_X);
        Lock::CollectionLock collLock(opCtx, toNs, MODE_X);

        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, toNs)) {
            uasserted(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while cloning collection " << from << " to "
                                    << to << " (as capped)");
        }

        Database* const db = autoColl.getDb();
        if (!db) {
            uasserted(ErrorCodes::NamespaceNotFound,
                      str::stream() << "database " << dbName.toStringForErrorMsg() << " not found");
        }

        cloneCollectionAsCapped(opCtx, db, fromNs, toNs, size, temp);
        return true;
    }

} cmdCloneCollectionAsCapped;

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
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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
        auto size = cmdObj.getField("size").safeNumberLong();

        uassert(ErrorCodes::InvalidOptions, "invalid command spec", size != 0);

        convertToCapped(opCtx, nss, size);
        return true;
    }

} cmdConvertToCapped;

}  // namespace
}  // namespace mongo

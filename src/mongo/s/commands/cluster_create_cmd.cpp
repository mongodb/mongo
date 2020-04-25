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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

class CreateCmd : public BasicCommand {
public:
    CreateCmd() : BasicCommand("create") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        return AuthorizationSession::get(client)->checkAuthForCreate(nss, cmdObj, true);
    }

    void checkCollectionOptions(OperationContext* opCtx,
                                const NamespaceString& ns,
                                const CollectionOptions& options) {
        auto dbName = ns.db();
        auto dbInfo = uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName));
        BSONObjBuilder listCollCmd;
        listCollCmd.append("listCollections", 1);
        listCollCmd.append("filter", BSON("name" << ns.coll()));

        auto response = executeCommandAgainstDatabasePrimary(
            opCtx,
            dbName,
            dbInfo,
            CommandHelpers::filterCommandRequestForPassthrough(listCollCmd.obj()),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent);
        uassertStatusOK(response.swResponse);
        auto responseData = response.swResponse.getValue().data;
        auto listCollectionsStatus = mongo::getStatusFromCommandResult(responseData);
        uassertStatusOK(listCollectionsStatus);
        auto cursorObj = responseData["cursor"].Obj();
        auto collections = cursorObj["firstBatch"].Obj();
        BSONObjIterator collIter(collections);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "cannot find ns: " << ns.ns(),
                collIter.more());

        auto collectionDetails = collIter.next();
        CollectionOptions actualOptions =
            uassertStatusOK(CollectionOptions::parse(collectionDetails["options"].Obj()));
        // TODO: SERVER-33048 check idIndex field

        uassert(ErrorCodes::NamespaceExists,
                str::stream() << "ns: " << ns.ns() << " already exists with different options: "
                              << actualOptions.toBSON(),
                options.matchesStorageOptions(
                    actualOptions, CollatorFactoryInterface::get(opCtx->getServiceContext())));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        createShardDatabase(opCtx, dbName);

        uassert(ErrorCodes::InvalidOptions,
                "specify size:<n> when capped is true",
                !cmdObj["capped"].trueValue() || cmdObj["size"].isNumber());
        uassert(ErrorCodes::InvalidOptions,
                "the 'temp' field is an invalid option",
                !cmdObj.hasField("temp"));

        // Manually forward the create collection command to the primary shard.
        const auto dbInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName));
        auto response = executeCommandAgainstDatabasePrimary(
            opCtx,
            dbName,
            dbInfo,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent);


        uassertStatusOK(response.swResponse);
        const auto createStatus =
            mongo::getStatusFromCommandResult(response.swResponse.getValue().data);
        if (createStatus == ErrorCodes::NamespaceExists && !opCtx->inMultiDocumentTransaction()) {
            // NamespaceExists will cause multi-document transactions to implicitly abort, so
            // mongos should surface this error to the client.
            CollectionOptions options = uassertStatusOK(CollectionOptions::parse(cmdObj));
            checkCollectionOptions(opCtx, nss, options);
        } else {
            uassertStatusOK(createStatus);
        }
        uassertStatusOK(
            getWriteConcernStatusFromCommandResult(response.swResponse.getValue().data));
        return true;
    }

} createCmd;

}  // namespace
}  // namespace mongo

/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/query/store_possible_cursor.h"

namespace mongo {
namespace {

bool cursorCommandPassthroughPrimaryShard(OperationContext* opCtx,
                                          StringData dbName,
                                          const CachedDatabaseInfo& dbInfo,
                                          const BSONObj& cmdObj,
                                          const NamespaceString& nss,
                                          BSONObjBuilder* out,
                                          const PrivilegeVector& privileges) {
    auto response = executeCommandAgainstDatabasePrimary(
        opCtx,
        dbName,
        dbInfo,
        CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent);
    const auto cmdResponse = uassertStatusOK(std::move(response.swResponse));

    auto transformedResponse = uassertStatusOK(
        storePossibleCursor(opCtx,
                            dbInfo.primaryId(),
                            *response.shardHostAndPort,
                            cmdResponse.data,
                            nss,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            Grid::get(opCtx)->getCursorManager(),
                            privileges));

    CommandHelpers::filterCommandReplyForPassthrough(transformedResponse, out);
    return true;
}

BSONObj rewriteCommandForListingOwnCollections(OperationContext* opCtx,
                                               const std::string& dbName,
                                               const BSONObj& cmdObj) {
    mutablebson::Document rewrittenCmdObj(cmdObj);
    mutablebson::Element ownCollections =
        mutablebson::findFirstChildNamed(rewrittenCmdObj.root(), "authorizedCollections");

    AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());

    // We must strip $ownCollections from the delegated command.
    uassertStatusOK(ownCollections.remove());

    BSONObj collectionFilter;

    // Extract and retain any previous filter
    mutablebson::Element oldFilter =
        mutablebson::findFirstChildNamed(rewrittenCmdObj.root(), "filter");

    // Make a new filter, containing a $and array.
    mutablebson::Element newFilter = rewrittenCmdObj.makeElementObject("filter");
    mutablebson::Element newFilterAnd = rewrittenCmdObj.makeElementArray("$and");
    uassertStatusOK(newFilter.pushBack(newFilterAnd));

    mutablebson::Element newFilterOr = rewrittenCmdObj.makeElementArray("$or");
    mutablebson::Element newFilterOrObj = rewrittenCmdObj.makeElementObject("");
    uassertStatusOK(newFilterOrObj.pushBack(newFilterOr));
    uassertStatusOK(newFilterAnd.pushBack(newFilterOrObj));

    // DB resource grants all non-system collections, so filter out system collections. This is done
    // inside the $or, since some system collections might be granted specific privileges.
    if (authzSession->isAuthorizedForAnyActionOnResource(
            ResourcePattern::forDatabaseName(dbName))) {
        mutablebson::Element systemCollectionsFilter = rewrittenCmdObj.makeElementObject(
            "", BSON("name" << BSON("$regex" << BSONRegEx("^(?!system\\.)"))));
        uassertStatusOK(newFilterOr.pushBack(systemCollectionsFilter));
    }

    // Compute the set of collection names which would be permissible to return.
    std::set<std::string> collectionNames;
    for (UserNameIterator nameIter = authzSession->getAuthenticatedUserNames(); nameIter.more();
         nameIter.next()) {
        User* authUser = authzSession->lookupUser(*nameIter);
        const User::ResourcePrivilegeMap& resourcePrivilegeMap = authUser->getPrivileges();
        for (const std::pair<ResourcePattern, Privilege>& resourcePrivilege :
             resourcePrivilegeMap) {
            const auto& resource = resourcePrivilege.first;
            if (resource.isCollectionPattern() ||
                (resource.isExactNamespacePattern() && resource.databaseToMatch() == dbName)) {
                collectionNames.emplace(resource.collectionToMatch().toString());
            }
        }
    }

    // Construct a new filter predicate which returns only collections we were found to have
    // privileges for.
    BSONObjBuilder predicateBuilder;
    BSONObjBuilder nameBuilder(predicateBuilder.subobjStart("name"));
    BSONArrayBuilder setBuilder(nameBuilder.subarrayStart("$in"));

    // Load the de-duplicated set into a BSON array
    for (StringData collectionName : collectionNames) {
        setBuilder << collectionName;
    }
    setBuilder.done();
    nameBuilder.done();

    collectionFilter = predicateBuilder.obj();

    // Filter the results by our collection names.
    mutablebson::Element newFilterCollections =
        rewrittenCmdObj.makeElementObject("", collectionFilter);
    uassertStatusOK(newFilterOr.pushBack(newFilterCollections));

    // If there was a pre-existing filter, compose it with our new one.
    if (oldFilter.ok()) {
        uassertStatusOK(oldFilter.remove());
        uassertStatusOK(newFilterAnd.pushBack(oldFilter));
    }

    // Attach our new composite filter back onto the listCollections command object.
    uassertStatusOK(rewrittenCmdObj.root().pushBack(newFilter));

    return rewrittenCmdObj.getObject();
}

class CmdListCollections : public BasicCommand {
public:
    CmdListCollections() : BasicCommand("listCollections") {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        if (authzSession->checkAuthorizedToListCollections(dbname, cmdObj).isOK()) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to list collections on db: " << dbname);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

        const auto nss(NamespaceString::makeListCollectionsNSS(dbName));

        BSONObj newCmd = cmdObj;

        AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
        if (authzSession->getAuthorizationManager().isAuthEnabled() &&
            newCmd["authorizedCollections"].trueValue()) {
            newCmd = rewriteCommandForListingOwnCollections(opCtx, dbName, cmdObj);
        }

        auto dbInfoStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);
        if (!dbInfoStatus.isOK()) {
            return appendEmptyResultSet(opCtx, result, dbInfoStatus.getStatus(), nss.ns());
        }

        return cursorCommandPassthroughPrimaryShard(
            opCtx,
            dbName,
            dbInfoStatus.getValue(),
            applyReadWriteConcern(opCtx, this, newCmd),
            nss,
            &result,
            uassertStatusOK(AuthorizationSession::get(opCtx->getClient())
                                ->checkAuthorizedToListCollections(dbName, cmdObj)));
    }

} cmdListCollections;

}  // namespace
}  // namespace mongo

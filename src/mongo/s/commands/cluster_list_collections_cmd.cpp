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


#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/query/store_possible_cursor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

constexpr auto systemBucketsDot = "system.buckets."_sd;

bool cursorCommandPassthroughPrimaryShard(OperationContext* opCtx,
                                          const DatabaseName& dbName,
                                          const CachedDatabaseInfo& dbInfo,
                                          const BSONObj& cmdObj,
                                          const NamespaceString& nss,
                                          BSONObjBuilder* out,
                                          const PrivilegeVector& privileges) {
    // TODO SERVER-67411 change executeCommandAgainstDatabasePrimary to take in DatabaseName
    auto response = executeCommandAgainstDatabasePrimary(
        opCtx,
        dbName.toStringWithTenantId(),
        dbInfo,
        CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent);
    const auto cmdResponse = uassertStatusOK(std::move(response.swResponse));

    auto transformedResponse = uassertStatusOK(
        storePossibleCursor(opCtx,
                            dbInfo->getPrimary(),
                            *response.shardHostAndPort,
                            cmdResponse.data,
                            nss,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            Grid::get(opCtx)->getCursorManager(),
                            privileges));

    CommandHelpers::filterCommandReplyForPassthrough(transformedResponse, out);
    uassertStatusOK(getStatusFromCommandResult(out->asTempObj()));
    return true;
}

BSONObj rewriteCommandForListingOwnCollections(OperationContext* opCtx,
                                               const DatabaseName& dbName,
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
            ResourcePattern::forDatabaseName(dbName.toStringWithTenantId()))) {
        mutablebson::Element systemCollectionsFilter = rewrittenCmdObj.makeElementObject(
            "", BSON("name" << BSON("$regex" << BSONRegEx("^(?!system\\.)"))));
        uassertStatusOK(newFilterOr.pushBack(systemCollectionsFilter));
    }

    // system_buckets DB resource grants all system_buckets.* collections so create a filter to
    // include them
    if (authzSession->isAuthorizedForAnyActionOnResource(
            ResourcePattern::forAnySystemBucketsInDatabase(dbName.toStringWithTenantId())) ||
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnySystemBuckets())) {
        mutablebson::Element systemCollectionsFilter = rewrittenCmdObj.makeElementObject(
            "", BSON("name" << BSON("$regex" << BSONRegEx("^system\\.buckets\\."))));
        uassertStatusOK(newFilterOr.pushBack(systemCollectionsFilter));
    }

    // Compute the set of collection names which would be permissible to return.
    std::set<std::string> collectionNames;
    if (auto authUser = authzSession->getAuthenticatedUser()) {
        for (const auto& [resource, privilege] : authUser.get()->getPrivileges()) {
            if (resource.isCollectionPattern() ||
                (resource.isExactNamespacePattern() &&
                 resource.databaseToMatch() == dbName.toStringWithTenantId())) {
                collectionNames.emplace(resource.collectionToMatch().toString());
            }

            if (resource.isAnySystemBucketsCollectionInAnyDB() ||
                (resource.isExactSystemBucketsCollection() &&
                 resource.databaseToMatch() == dbName.toStringWithTenantId())) {
                collectionNames.emplace(systemBucketsDot + resource.collectionToMatch().toString());
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

    auto rewrittenCmd = rewrittenCmdObj.getObject();

    // Make sure the modified request still conforms to the IDL spec. We only want to run this while
    // testing because an error while parsing indicates an internal error, not something that should
    // surface to a user.
    if (getTestCommandsEnabled()) {
        ListCollections::parse(IDLParserContext("ListCollectionsForOwnCollections"), rewrittenCmd);
    }

    return rewrittenCmd;
}

class CmdListCollections : public BasicCommandWithRequestParser<CmdListCollections> {
public:
    using Request = ListCollections;

    const std::set<std::string>& apiVersions() const final {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const final {
        return false;
    }

    bool adminOnly() const final {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        return authzSession->checkAuthorizedToListCollections(dbname, cmdObj).getStatus();
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& output) final {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

        const auto nss(NamespaceString::makeListCollectionsNSS(dbName));

        BSONObj newCmd = cmdObj;

        const bool authorizedCollections = requestParser.request().getAuthorizedCollections();
        AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
        if (authzSession->getAuthorizationManager().isAuthEnabled() && authorizedCollections) {
            newCmd = rewriteCommandForListingOwnCollections(opCtx, dbName, cmdObj);
        }

        // TODO SERVER-67797 Change CatalogCache to use DatabaseName object
        auto dbInfoStatus =
            Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName.toStringWithTenantId());
        if (!dbInfoStatus.isOK()) {
            appendEmptyResultSet(opCtx, output, dbInfoStatus.getStatus(), nss.ns());
            return true;
        }

        return cursorCommandPassthroughPrimaryShard(
            opCtx,
            dbName,
            dbInfoStatus.getValue(),
            applyReadWriteConcern(opCtx, this, newCmd),
            nss,
            &output,
            // Use the original command object rather than the rewritten one to preserve whether
            // 'authorizedCollections' field is set.
            uassertStatusOK(
                AuthorizationSession::get(opCtx->getClient())
                    ->checkAuthorizedToListCollections(dbName.toStringWithTenantId(), cmdObj)));
    }

    void validateResult(const BSONObj& result) final {
        StringDataSet ignorableFields({ErrorReply::kOkFieldName});
        ListCollectionsReply::parse(IDLParserContext("ListCollectionsReply"),
                                    result.removeFields(ignorableFields));
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::ListCollections::kAuthorizationContract;
    }
} cmdListCollections;

}  // namespace
}  // namespace mongo

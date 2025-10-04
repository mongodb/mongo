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


#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/query/exec/store_possible_cursor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/string_map.h"

#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

constexpr auto systemBucketsDot = "system.buckets."_sd;

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
            ResourcePattern::forDatabaseName(dbName))) {
        mutablebson::Element systemCollectionsFilter = rewrittenCmdObj.makeElementObject(
            "", BSON("name" << BSON("$regex" << BSONRegEx("^(?!system\\.)"))));
        uassertStatusOK(newFilterOr.pushBack(systemCollectionsFilter));
    }

    // system_buckets DB resource grants all system_buckets.* collections so create a filter to
    // include them
    if (authzSession->isAuthorizedForAnyActionOnResource(
            ResourcePattern::forAnySystemBucketsInDatabase(dbName)) ||
        authzSession->isAuthorizedForAnyActionOnResource(
            ResourcePattern::forAnySystemBuckets(dbName.tenantId()))) {
        mutablebson::Element systemCollectionsFilter = rewrittenCmdObj.makeElementObject(
            "", BSON("name" << BSON("$regex" << BSONRegEx("^system\\.buckets\\."))));
        uassertStatusOK(newFilterOr.pushBack(systemCollectionsFilter));
    }

    // Compute the set of collection names which would be permissible to return.
    std::set<std::string> collectionNames;
    if (auto authUser = authzSession->getAuthenticatedUser()) {
        for (const auto& [resource, privilege] : authUser.value()->getPrivileges()) {
            if (resource.isCollectionPattern() ||
                (resource.isExactNamespacePattern() && resource.dbNameToMatch() == dbName)) {
                collectionNames.emplace(std::string{resource.collectionToMatch()});
            }

            if (resource.isAnySystemBucketsCollectionInAnyDB() ||
                (resource.isExactSystemBucketsCollection() && resource.dbNameToMatch() == dbName)) {
                collectionNames.emplace(systemBucketsDot +
                                        std::string{resource.collectionToMatch()});
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
        ListCollections::parse(rewrittenCmd, IDLParserContext("ListCollectionsForOwnCollections"));
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

    bool supportsRawData() const final {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const final {
        auto* authzSession = AuthorizationSession::get(opCtx->getClient());
        IDLParserContext ctxt("ListCollection",
                              auth::ValidatedTenancyScope::get(opCtx),
                              dbName.tenantId(),
                              SerializationContext::stateDefault());
        auto request = idl::parseCommandDocument<ListCollections>(cmdObj, ctxt);
        return authzSession->checkAuthorizedToListCollections(request).getStatus();
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
        if (AuthorizationManager::get(opCtx->getService())->isAuthEnabled() &&
            authorizedCollections) {
            newCmd = rewriteCommandForListingOwnCollections(opCtx, dbName, cmdObj);
        }

        // Use the original command object rather than the rewritten one to preserve whether
        // 'authorizedCollections' field is set.
        const auto& privileges =
            uassertStatusOK(AuthorizationSession::get(opCtx->getClient())
                                ->checkAuthorizedToListCollections(requestParser.request()));

        const auto cmdToSend = applyReadWriteConcern(opCtx, this, newCmd);

        try {
            sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), nss.dbName());
            router.route(
                opCtx,
                Request::kCommandName,
                [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                    auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                        opCtx,
                        dbName,
                        dbInfo,
                        CommandHelpers::filterCommandRequestForPassthrough(cmdToSend),
                        ReadPreferenceSetting::get(opCtx),
                        Shard::RetryPolicy::kIdempotent);
                    const auto cmdResponse = uassertStatusOK(std::move(response.swResponse));

                    auto transformedResponse = uassertStatusOK(storePossibleCursor(
                        opCtx,
                        dbInfo->getPrimary(),
                        *response.shardHostAndPort,
                        cmdResponse.data,
                        nss,
                        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                        Grid::get(opCtx)->getCursorManager(),
                        privileges));

                    CommandHelpers::filterCommandReplyForPassthrough(transformedResponse, &output);
                    uassertStatusOK(getStatusFromCommandResult(output.asTempObj()));
                });
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& e) {
            appendEmptyResultSet(opCtx, output, e.toStatus(), nss);
            return true;
        }
        return true;
    }

    void validateResult(const BSONObj& result) final {
        StringDataSet ignorableFields({ErrorReply::kOkFieldName});
        ListCollectionsReply::parse(result.removeFields(ignorableFields),
                                    IDLParserContext("ListCollectionsReply"));
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::ListCollections::kAuthorizationContract;
    }
};
MONGO_REGISTER_COMMAND(CmdListCollections).forRouter();

}  // namespace
}  // namespace mongo

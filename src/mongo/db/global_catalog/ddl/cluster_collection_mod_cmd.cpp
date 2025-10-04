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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/ddl/coll_mod_reply_validation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <set>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

constexpr auto kRawFieldName = "raw"_sd;
constexpr auto kWriteConcernErrorFieldName = "writeConcernError"_sd;
constexpr auto kTopologyVersionFieldName = "topologyVersion"_sd;

class CollectionModCmd : public BasicCommandWithRequestParser<CollectionModCmd> {
public:
    using Request = CollMod;
    using Reply = CollModReply;

    CollectionModCmd() : BasicCommandWithRequestParser() {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* client = opCtx->getClient();
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        return auth::checkAuthForCollMod(client->getOperationContext(),
                                         AuthorizationSession::get(client),
                                         nss,
                                         cmdObj,
                                         true,
                                         SerializationContext::stateCommandRequest());
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) final {
        const auto& cmd = requestParser.request();
        auto nss = cmd.getNamespace();
        LOGV2_DEBUG(22748, 1, "CMD: collMod", logAttrs(nss), "command"_attr = redact(cmdObj));

        if (cmd.getValidator() || cmd.getValidationLevel() || cmd.getValidationAction()) {
            // Check for config.settings in the user command since a validator is allowed
            // internally on this collection but the user may not modify the validator.
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Document validators not allowed on system collection "
                                  << nss.toStringForErrorMsg(),
                    nss != NamespaceString::kConfigSettingsNamespace);
        }

        sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), cmd.getDbName());

        try {
            return router.route(
                opCtx,
                Request::kCommandName,
                [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                    ShardsvrCollMod collModCommand(nss);
                    collModCommand.setCollModRequest(cmd.getCollModRequest());
                    generic_argument_util::setMajorityWriteConcern(collModCommand,
                                                                   &opCtx->getWriteConcern());
                    auto cmdResponse = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                        opCtx,
                        dbName,
                        dbInfo,
                        collModCommand.toBSON(),
                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                        Shard::RetryPolicy::kIdempotent);

                    const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                    CommandHelpers::filterCommandReplyForPassthrough(remoteResponse.data, &result);
                    return remoteResponse.isOK();
                });
        } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // Throw a CollectionUUIDMismatchInfo instead of a NamespaceNotFound error if the
            // collectionUUID was provided.
            uassert(CollectionUUIDMismatchInfo(cmd.getDbName(),
                                               *cmd.getCollectionUUID(),
                                               std::string{nss.coll()},
                                               boost::none),
                    "Database does not exist",
                    !cmd.getCollectionUUID());
            throw;
        }
    }

    void validateResult(const BSONObj& resultObj) final {
        auto ctx = IDLParserContext("CollModReply");
        if (checkIsErrorStatus(resultObj, ctx)) {
            return;
        }

        StringDataSet ignorableFields({kWriteConcernErrorFieldName,
                                       ErrorReply::kOkFieldName,
                                       kTopologyVersionFieldName,
                                       kRawFieldName});
        auto reply = Reply::parse(resultObj.removeFields(ignorableFields), ctx);
        coll_mod_reply_validation::validateReply(reply);

        if (!resultObj.hasField(kRawFieldName)) {
            return;
        }

        const auto& rawData = resultObj[kRawFieldName];
        if (!ctx.checkAndAssertType(rawData, BSONType::object)) {
            return;
        }

        auto rawCtx = IDLParserContext(kRawFieldName, &ctx);
        for (const auto& element : rawData.Obj()) {
            if (!rawCtx.checkAndAssertType(element, BSONType::object)) {
                return;
            }

            const auto& shardReply = element.Obj();
            if (!checkIsErrorStatus(shardReply, ctx)) {
                auto reply = Reply::parse(shardReply.removeFields(ignorableFields), ctx);
                coll_mod_reply_validation::validateReply(reply);
            }
        }
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::CollMod::kAuthorizationContract;
    }
};
MONGO_REGISTER_COMMAND(CollectionModCmd).forRouter();

}  // namespace
}  // namespace mongo

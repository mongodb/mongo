// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
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
#include "mongo/db/database_name_util.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/db/shard_role/ddl/coll_mod_reply_validation.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <set>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kRawFieldName = "raw"sv;
constexpr auto kWriteConcernErrorFieldName = "writeConcernError"sv;
constexpr auto kTopologyVersionFieldName = "topologyVersion"sv;

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

        sharding::router::DBPrimaryRouter router(opCtx, cmd.getDbName());

        try {
            return router.route(
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

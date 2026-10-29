// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
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
#include "mongo/db/shard_role/ddl/drop_indexes_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"

#include <set>
#include <string>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kRawFieldName = "raw"sv;

class DropIndexesCmd : public BasicCommandWithRequestParser<DropIndexesCmd> {
public:
    using Request = DropIndexes;
    using Reply = DropIndexesReply;

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsRawData() const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::dropIndex)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    void validateResult(const BSONObj& resultObj) final {
        auto ctx = IDLParserContext("DropIndexesReply");
        if (!checkIsErrorStatus(resultObj, ctx)) {
            Reply::parse(resultObj.removeField(kRawFieldName), ctx);
            if (resultObj.hasField(kRawFieldName)) {
                const auto& rawData = resultObj[kRawFieldName];
                if (ctx.checkAndAssertType(rawData, BSONType::object)) {
                    for (const auto& element : rawData.Obj()) {
                        const auto& shardReply = element.Obj();
                        if (!checkIsErrorStatus(shardReply, ctx)) {
                            Reply::parse(shardReply, ctx);
                        }
                    }
                }
            }
        }
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& output) final {
        auto nss = requestParser.request().getNamespace();

        uassert(ErrorCodes::IllegalOperation,
                "Cannot drop indexes in 'config' database in sharded cluster",
                nss.dbName() != DatabaseName::kConfig);

        uassert(ErrorCodes::IllegalOperation,
                "Cannot drop indexes in 'admin' database in sharded cluster",
                nss.dbName() != DatabaseName::kAdmin);

        LOGV2_DEBUG(22751, 1, "CMD: dropIndexes", logAttrs(nss), "command"_attr = redact(cmdObj));

        ShardsvrDropIndexes shardsvrDropIndexCmd(nss);
        shardsvrDropIndexCmd.setDropIndexesRequest(requestParser.request().getDropIndexesRequest());
        generic_argument_util::setMajorityWriteConcern(shardsvrDropIndexCmd,
                                                       &opCtx->getWriteConcern());

        sharding::router::DBPrimaryRouter router(opCtx, dbName);
        router.route(
            Request::kCommandName, [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                auto cmdResponse = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                    opCtx,
                    dbName,
                    dbInfo,
                    shardsvrDropIndexCmd.toBSON(),
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    Shard::RetryPolicy::kNotIdempotent);

                const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                CommandHelpers::filterCommandReplyForPassthrough(remoteResponse.data, &output);
            });
        return true;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::DropIndexes::kAuthorizationContract;
    }
};
MONGO_REGISTER_COMMAND(DropIndexesCmd).forRouter();

}  // namespace
}  // namespace mongo

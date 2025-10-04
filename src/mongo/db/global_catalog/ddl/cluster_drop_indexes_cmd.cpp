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
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/local_catalog/ddl/drop_indexes_gen.h"
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

#include <set>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

constexpr auto kRawFieldName = "raw"_sd;

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

        sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), dbName);
        router.route(
            opCtx,
            Request::kCommandName,
            [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
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

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

#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

constexpr auto kRawFieldName = "raw"_sd;

class DropIndexesCmd : public BasicCommandWithRequestParser<DropIndexesCmd> {
public:
    using Request = DropIndexes;
    using Reply = DropIndexesReply;

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::dropIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    void validateResult(const BSONObj& resultObj) final {
        auto ctx = IDLParserContext("DropIndexesReply");
        if (!checkIsErrorStatus(resultObj, ctx)) {
            Reply::parse(ctx, resultObj.removeField(kRawFieldName));
            if (resultObj.hasField(kRawFieldName)) {
                const auto& rawData = resultObj[kRawFieldName];
                if (ctx.checkAndAssertType(rawData, Object)) {
                    for (const auto& element : rawData.Obj()) {
                        const auto& shardReply = element.Obj();
                        if (!checkIsErrorStatus(shardReply, ctx)) {
                            Reply::parse(ctx, shardReply);
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
                nss.db() != NamespaceString::kConfigDb);

        uassert(ErrorCodes::IllegalOperation,
                "Cannot drop indexes in 'admin' database in sharded cluster",
                nss.db() != NamespaceString::kAdminDb);

        LOGV2_DEBUG(22751,
                    1,
                    "dropIndexes: {namespace} cmd: {command}",
                    "CMD: dropIndexes",
                    "namespace"_attr = nss,
                    "command"_attr = redact(cmdObj));

        ShardsvrDropIndexes shardsvrDropIndexCmd(nss);
        shardsvrDropIndexCmd.setDropIndexesRequest(requestParser.request().getDropIndexesRequest());

        // TODO SERVER-67797 Change CatalogCache to use DatabaseName object
        const CachedDatabaseInfo dbInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName.toStringWithTenantId()));

        // TODO SERVER-67411 change executeCommandAgainstDatabasePrimary to take in DatabaseName
        auto cmdResponse = executeCommandAgainstDatabasePrimary(
            opCtx,
            dbName.toStringWithTenantId(),
            dbInfo,
            CommandHelpers::appendMajorityWriteConcern(shardsvrDropIndexCmd.toBSON({}),
                                                       opCtx->getWriteConcern()),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kNotIdempotent);

        const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
        CommandHelpers::filterCommandReplyForPassthrough(remoteResponse.data, &output);
        return true;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::DropIndexes::kAuthorizationContract;
    }
} dropIndexesCmd;

}  // namespace
}  // namespace mongo

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
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/coll_mod_reply_validation.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

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

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));
        return AuthorizationSession::get(client)->checkAuthForCollMod(nss, cmdObj, true);
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const std::string& db,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) final {
        auto cmd = requestParser.request();
        auto nss = cmd.getNamespace();
        LOGV2_DEBUG(22748,
                    1,
                    "collMod: {namespace} cmd: {command}",
                    "CMD: collMod",
                    "namespace"_attr = nss,
                    "command"_attr = redact(cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            cmd.getDbName(),
            nss,
            routingInfo,
            applyReadWriteConcern(
                opCtx,
                this,
                CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON(BSONObj()))),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kNoRetry,
            BSONObj() /* query */,
            BSONObj() /* collation */);
        std::string errmsg;
        auto ok = appendRawResponses(opCtx, &errmsg, &result, std::move(shardResponses)).responseOK;
        if (!errmsg.empty()) {
            CommandHelpers::appendSimpleCommandStatus(result, ok, errmsg);
        }

        return ok;
    }

    void validateResult(const BSONObj& resultObj) final {
        auto ctx = IDLParserErrorContext("CollModReply");
        StringDataSet ignorableFields(
            {kWriteConcernErrorFieldName, ErrorReply::kOkFieldName, kTopologyVersionFieldName});
        if (!checkIsErrorStatus(resultObj, ctx)) {
            if (resultObj.hasField(kRawFieldName)) {
                const auto& rawData = resultObj[kRawFieldName];
                if (ctx.checkAndAssertType(rawData, Object)) {
                    for (const auto& element : rawData.Obj()) {
                        const auto& shardReply = element.Obj();
                        if (!checkIsErrorStatus(shardReply, ctx)) {
                            auto reply =
                                Reply::parse(ctx, shardReply.removeFields(ignorableFields));
                            coll_mod_reply_validation::validateReply(reply);
                        }
                    }
                }
            } else {
                auto reply = Reply::parse(ctx, resultObj.removeFields(ignorableFields));
                coll_mod_reply_validation::validateReply(reply);
            }
        }
    }

} collectionModCmd;

}  // namespace
}  // namespace mongo

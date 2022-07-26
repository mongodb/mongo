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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/create_indexes_gen.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

constexpr auto kRawFieldName = "raw"_sd;
constexpr auto kWriteConcernErrorFieldName = "writeConcernError"_sd;
constexpr auto kTopologyVersionFieldName = "topologyVersion"_sd;

class CreateIndexesCmd : public BasicCommandWithRequestParser<CreateIndexesCmd> {
public:
    using Request = CreateIndexesCommand;
    using Reply = CreateIndexesReply;

    const std::set<std::string>& apiVersions() const final {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const final {
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), ActionType::createIndex));
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser&,
                              BSONObjBuilder& output) final {
        // TODO SERVER-67519 Change CommandHelpers::parseNs* methods to construct NamespaceStrings
        // with tenantId
        const NamespaceString nss(
            CommandHelpers::parseNsCollectionRequired(dbName.toStringWithTenantId(), cmdObj));
        LOGV2_DEBUG(22750,
                    1,
                    "createIndexes: {namespace} cmd: {command}",
                    "CMD: createIndexes",
                    "namespace"_attr = nss,
                    "command"_attr = redact(cmdObj));

        // TODO SERVER-67798 Change cluster::createDatabase to use DatabaseName
        cluster::createDatabase(opCtx, dbName.toStringWithTenantId());

        auto targeter = ChunkManagerTargeter(opCtx, nss);
        auto routingInfo = targeter.getRoutingInfo();
        auto cmdToBeSent = cmdObj;
        if (targeter.timeseriesNamespaceNeedsRewrite(nss)) {
            cmdToBeSent = timeseries::makeTimeseriesCommand(
                cmdToBeSent, nss, getName(), CreateIndexesCommand::kIsTimeseriesNamespaceFieldName);
        }

        auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            nss.db(),
            targeter.getNS(),
            routingInfo,
            CommandHelpers::filterCommandRequestForPassthrough(
                applyReadWriteConcern(opCtx, this, cmdToBeSent)),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kNoRetry,
            BSONObj() /* query */,
            BSONObj() /* collation */);

        std::string errmsg;
        const bool ok =
            appendRawResponses(opCtx, &errmsg, &output, std::move(shardResponses)).responseOK;
        CommandHelpers::appendSimpleCommandStatus(output, ok, errmsg);

        if (ok) {
            LOGV2(5706400, "Indexes created", "namespace"_attr = nss);
        }

        return ok;
    }

    /**
     * Response should either be "ok" and contain just 'raw' which is a dictionary of
     * CreateIndexesReply (with optional 'ok' and 'writeConcernError' fields).
     * or it should be "not ok" and contain an 'errmsg' and possibly a 'writeConcernError'.
     * 'code' & 'codeName' are permitted in either scenario, but non-zero 'code' indicates "not ok".
     */
    void validateResult(const BSONObj& result) final {
        auto ctx = IDLParserContext("createIndexesReply");
        if (checkIsErrorStatus(result, ctx)) {
            return;
        }

        StringDataSet ignorableFields({kWriteConcernErrorFieldName,
                                       ErrorReply::kOkFieldName,
                                       kTopologyVersionFieldName,
                                       kRawFieldName});
        Reply::parse(ctx, result.removeFields(ignorableFields));
        if (!result.hasField(kRawFieldName)) {
            return;
        }

        const auto& rawData = result[kRawFieldName];
        if (!ctx.checkAndAssertType(rawData, Object)) {
            return;
        }

        auto rawCtx = IDLParserContext(kRawFieldName, &ctx);
        for (const auto& element : rawData.Obj()) {
            if (!rawCtx.checkAndAssertType(element, Object)) {
                return;
            }

            const auto& shardReply = element.Obj();
            if (!checkIsErrorStatus(shardReply, ctx)) {
                Reply::parse(ctx, shardReply.removeFields(ignorableFields));
            }
        }
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::CreateIndexesCommand::kAuthorizationContract;
    }
} createIndexesCmd;

}  // namespace
}  // namespace mongo

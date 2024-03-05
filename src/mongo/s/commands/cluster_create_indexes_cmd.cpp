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


#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

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
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/commands.h"
#include "mongo/db/create_indexes_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/collection_uuid_mismatch.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/string_map.h"

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

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::createIndex)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
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
                              const RequestParser& requestParser,
                              BSONObjBuilder& output) final {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        LOGV2_DEBUG(22750, 1, "CMD: createIndexes", logAttrs(nss), "command"_attr = redact(cmdObj));

        auto targeter = CollectionRoutingInfoTargeter(opCtx, nss);
        auto routingInfo = targeter.getRoutingInfo();
        auto cmdToBeSent = cmdObj;
        if (targeter.timeseriesNamespaceNeedsRewrite(nss)) {
            const auto& request = requestParser.request();
            if (auto uuid = request.getCollectionUUID()) {
                auto status = Status(CollectionUUIDMismatchInfo(
                                         nss.dbName(), *uuid, nss.coll().toString(), boost::none),
                                     "'collectionUUID' is specified for a time-series view "
                                     "namespace; views do not have UUIDs");
                uassertStatusOK(populateCollectionUUIDMismatch(opCtx, status));
                MONGO_UNREACHABLE_TASSERT(8549600);
            }

            cmdToBeSent = timeseries::makeTimeseriesCommand(
                cmdToBeSent, nss, getName(), CreateIndexesCommand::kIsTimeseriesNamespaceFieldName);
        }

        auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            nss.dbName(),
            targeter.getNS(),
            routingInfo,
            CommandHelpers::filterCommandRequestForPassthrough(
                applyReadWriteConcern(opCtx, this, cmdToBeSent)),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kNoRetry,
            BSONObj() /*query*/,
            BSONObj() /*collation*/,
            boost::none /*letParameters*/,
            boost::none /*runtimeConstants*/);

        BSONObjBuilder rawResBuilder;
        std::string errmsg;
        bool isShardedCollection = routingInfo.cm.isSharded();
        const bool ok =
            appendRawResponses(opCtx, &errmsg, &rawResBuilder, shardResponses, isShardedCollection)
                .responseOK;

        if (!isShardedCollection && ok) {
            CommandHelpers::filterCommandReplyForPassthrough(
                shardResponses[0].swResponse.getValue().data, &output);
        }

        output.appendElements(rawResBuilder.obj());
        CommandHelpers::appendSimpleCommandStatus(output, ok, errmsg);

        if (ok) {
            LOGV2(5706400, "Indexes created", logAttrs(nss));
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
};
MONGO_REGISTER_COMMAND(CreateIndexesCmd).forRouter();

}  // namespace
}  // namespace mongo

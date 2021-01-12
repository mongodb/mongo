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
#include "mongo/db/commands.h"
#include "mongo/db/create_indexes_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

constexpr auto kRawFieldName = "raw"_sd;
constexpr auto kWriteConcernErrorFieldName = "writeConcernError"_sd;
constexpr auto kTopologyVersionFieldName = "topologyVersion"_sd;

class CreateIndexesCmd : public BasicCommandWithRequestParser<CreateIndexesCmd> {
public:
    using Request = CreateIndexesCommand;

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
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), {ActionType::createIndex}));
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const std::string& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser&,
                              BSONObjBuilder& output) final {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        LOGV2_DEBUG(22750,
                    1,
                    "createIndexes: {namespace} cmd: {command}",
                    "CMD: createIndexes",
                    "namespace"_attr = nss,
                    "command"_attr = redact(cmdObj));

        createShardDatabase(opCtx, dbName);

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            nss.db(),
            nss,
            routingInfo,
            CommandHelpers::filterCommandRequestForPassthrough(
                applyReadWriteConcern(opCtx, this, cmdObj)),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kNoRetry,
            BSONObj() /* query */,
            BSONObj() /* collation */);

        std::string errmsg;
        const bool ok =
            appendRawResponses(opCtx, &errmsg, &output, std::move(shardResponses)).responseOK;
        if (!errmsg.empty()) {
            CommandHelpers::appendSimpleCommandStatus(output, ok, errmsg);
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
        BSONElement rawElem;
        bool ok = true, hasErrMsg = false;

        for (auto elem : result) {
            const auto fieldName = elem.fieldNameStringData();
            if (fieldName == kRawFieldName) {
                rawElem = elem;
                uassert(ErrorCodes::BadValue,
                        str::stream()
                            << "'raw' field must be an object, got: " << typeName(elem.type()),
                        elem.type() == Object);
            } else if (fieldName == ErrorReply::kCodeFieldName) {
                uassert(ErrorCodes::BadValue,
                        str::stream() << "Reply contained non-numeric status code: " << elem,
                        elem.isNumber());
                ok = ok & (elem.numberInt() != 0);
            } else if (fieldName == ErrorReply::kOkFieldName) {
                ok = ok & elem.trueValue();
            } else if (fieldName == ErrorReply::kErrmsgFieldName) {
                hasErrMsg = true;
            } else if ((fieldName == ErrorReply::kCodeNameFieldName) ||
                       (fieldName == kWriteConcernErrorFieldName)) {
                // Ignorable field.
            } else {
                uasserted(ErrorCodes::BadValue,
                          str::stream() << "Invalid field in reply: " << fieldName);
            }
        }

        if (ok) {
            uassert(
                ErrorCodes::BadValue, "Error message field present for 'ok' result", !hasErrMsg);
            uassert(ErrorCodes::BadValue, "Missing field in reply: raw", !rawElem.eoo());

            invariant(rawElem.type() == Object);  // Validated in field loop above.
            IDLParserErrorContext ctx("createIndexesReply");
            StringDataSet ignorableFields(
                {kWriteConcernErrorFieldName, ErrorReply::kOkFieldName, kTopologyVersionFieldName});
            for (auto elem : rawElem.Obj()) {
                uassert(ErrorCodes::FailedToParse,
                        str::stream() << "Response from shard must be an object, found: "
                                      << typeName(elem.type()),
                        elem.type() == Object);
                try {
                    // 'ok' is a permissable part of an reply even though it's not
                    // a formal part of the command reply.
                    CreateIndexesReply::parse(ctx, elem.Obj().removeFields(ignorableFields));
                } catch (const DBException& ex) {
                    uasserted(ex.code(),
                              str::stream()
                                  << "Failed parsing response from shard: " << ex.reason());
                }
            }
        } else {
            uassert(
                ErrorCodes::BadValue, "Error message field missing for 'not ok' result", hasErrMsg);
        }
    }
} createIndexesCmd;

}  // namespace
}  // namespace mongo

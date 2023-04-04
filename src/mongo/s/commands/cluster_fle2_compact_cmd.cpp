/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

class ClusterCompactStructuredEncryptionDataCmd final
    : public TypedCommand<ClusterCompactStructuredEncryptionDataCmd> {
public:
    using Request = CompactStructuredEncryptionData;
    using Reply = CompactStructuredEncryptionData::Reply;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx);

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to compact structured encryption data",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(request().getNamespace()),
                        ActionType::compactStructuredEncryptionData));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }

    std::set<StringData> sensitiveFieldNames() const final {
        return {CompactStructuredEncryptionData::kCompactionTokensFieldName};
    }
} clusterCompactStructuredEncryptionDataCmd;

using Cmd = ClusterCompactStructuredEncryptionDataCmd;
Cmd::Reply Cmd::Invocation::typedRun(OperationContext* opCtx) {
    CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

    auto nss = request().getNamespace();
    const auto dbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db()));

    // Rewrite command verb to _shardSvrCompactStructuredEnccryptionData.
    auto cmd = request().toBSON({});
    BSONObjBuilder req;
    for (const auto& elem : cmd) {
        if (elem.fieldNameStringData() == Request::kCommandName) {
            req.appendAs(elem, "_shardsvrCompactStructuredEncryptionData");
        } else {
            req.append(elem);
        }
    }

    auto response = uassertStatusOK(
        executeCommandAgainstDatabasePrimary(
            opCtx,
            nss.db(),
            dbInfo,
            CommandHelpers::appendMajorityWriteConcern(req.obj(), opCtx->getWriteConcern()),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent)
            .swResponse);

    BSONObjBuilder result;
    CommandHelpers::filterCommandReplyForPassthrough(response.data, &result);

    auto reply = result.obj();
    uassertStatusOK(getStatusFromCommandResult(reply));
    return Reply::parse(IDLParserContext{Request::kCommandName}, reply.removeField("ok"_sd));
}

}  // namespace
}  // namespace mongo

/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/commands/fle2_get_count_info_command_gen.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

/**
 * Retrieve a set of tags from ESC. Returns a count suitable for either insert or query.
 *
 * Always routes to primary shard for a database because ESC is pinned to primary shard and ESC is
 * not sharded.
 */
class ClusterGetQueryableEncryptionCountInfoCmd final
    : public TypedCommand<ClusterGetQueryableEncryptionCountInfoCmd> {
public:
    using Request = GetQueryableEncryptionCountInfo;
    using Reply = GetQueryableEncryptionCountInfo::Reply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return BasicCommand::AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return false;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    std::set<StringData> sensitiveFieldNames() const final {
        return {GetQueryableEncryptionCountInfo::kTokensFieldName};
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx);

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to read tags",
                    as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                         ActionType::internal));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }
    };

} ClusterGetQueryableEncryptionCountInfoCmd;

ClusterGetQueryableEncryptionCountInfoCmd::Reply
ClusterGetQueryableEncryptionCountInfoCmd::Invocation::typedRun(OperationContext* opCtx) {

    CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

    auto nss = request().getNamespace();
    const auto dbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db()));

    auto response = uassertStatusOK(
        executeCommandAgainstDatabasePrimary(
            opCtx,
            nss.db(),
            dbInfo,
            applyReadWriteConcern(
                opCtx,
                this,
                CommandHelpers::filterCommandRequestForPassthrough(unparsedRequest().body)),
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

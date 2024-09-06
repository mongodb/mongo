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

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fle2_cleanup_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class ClusterCleanupStructuredEncryptionDataCmd final
    : public TypedCommand<ClusterCleanupStructuredEncryptionDataCmd> {
public:
    using Request = CleanupStructuredEncryptionData;
    using Reply = CleanupStructuredEncryptionData::Reply;

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
                    "Not authorized to cleanup structured encryption data",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(request().getNamespace()),
                        ActionType::cleanupStructuredEncryptionData));
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
        return {CleanupStructuredEncryptionData::kCleanupTokensFieldName};
    }
};
MONGO_REGISTER_COMMAND(ClusterCleanupStructuredEncryptionDataCmd).forRouter();

using Cmd = ClusterCleanupStructuredEncryptionDataCmd;
Cmd::Reply Cmd::Invocation::typedRun(OperationContext* opCtx) {
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
    }

    auto nss = request().getNamespace();
    const auto dbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.dbName()));

    auto req = request();
    generic_argument_util::setMajorityWriteConcern(req, &opCtx->getWriteConcern());

    // Rewrite command verb to _shardSvrCleanupStructuredEnccryptionData.
    auto cmd = req.toBSON();
    BSONObjBuilder reqBuilder;
    for (const auto& elem : cmd) {
        if (elem.fieldNameStringData() == Request::kCommandName) {
            reqBuilder.appendAs(elem, "_shardsvrCleanupStructuredEncryptionData");
        } else {
            reqBuilder.append(elem);
        }
    }

    auto response =
        uassertStatusOK(executeDDLCoordinatorCommandAgainstDatabasePrimary(
                            opCtx,
                            nss.dbName(),
                            dbInfo,
                            CommandHelpers::filterCommandRequestForPassthrough(reqBuilder.obj()),
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

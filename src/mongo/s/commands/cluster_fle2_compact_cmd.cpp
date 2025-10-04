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
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>

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
};
MONGO_REGISTER_COMMAND(ClusterCompactStructuredEncryptionDataCmd).forRouter();

using Cmd = ClusterCompactStructuredEncryptionDataCmd;
Cmd::Reply Cmd::Invocation::typedRun(OperationContext* opCtx) {
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    auto nss = request().getNamespace();

    auto req = request();
    generic_argument_util::setMajorityWriteConcern(req, &opCtx->getWriteConcern());

    sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), nss.dbName());
    return router.route(
        opCtx,
        Request::kCommandName,
        [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
            // Rewrite command verb to _shardSvrCompactStructuredEnccryptionData.
            auto cmd = req.toBSON();
            BSONObjBuilder reqBuilder;
            for (const auto& elem : cmd) {
                if (elem.fieldNameStringData() == Request::kCommandName) {
                    reqBuilder.appendAs(elem, "_shardsvrCompactStructuredEncryptionData");
                } else {
                    reqBuilder.append(elem);
                }
            }

            auto response = uassertStatusOK(
                executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                    opCtx,
                    nss.dbName(),
                    dbInfo,
                    CommandHelpers::filterCommandRequestForPassthrough(reqBuilder.obj()),
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    Shard::RetryPolicy::kIdempotent)
                    .swResponse);

            auto reply = CommandHelpers::filterCommandReplyForPassthrough(response.data);
            uassertStatusOK(getStatusFromCommandResult(reply));
            return Reply::parse(reply.removeField("ok"_sd),
                                IDLParserContext{Request::kCommandName});
        });
}

}  // namespace
}  // namespace mongo

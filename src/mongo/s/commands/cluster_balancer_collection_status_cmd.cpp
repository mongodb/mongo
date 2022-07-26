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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/balancer_collection_status_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class BalancerCollectionStatusCmd final : public TypedCommand<BalancerCollectionStatusCmd> {
public:
    using Request = BalancerCollectionStatus;
    using Response = BalancerCollectionStatusResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        StringData kStatusField = "status"_sd;

        Response typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            ConfigsvrBalancerCollectionStatus configsvrRequest(nss);
            configsvrRequest.setDbName(request().getDbName());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                "admin",
                configsvrRequest.toBSON({}),
                Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);

            return Response::parse(IDLParserContext("BalancerCollectionStatusResponse"),
                                   cmdResponse.response);
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::enableSharding));
        }
    };

    std::string help() const override {
        return "command to check whether the chunks of a given collection are in a quiesced state "
               "or there are any which need to be moved because of (1) draining shards, (2) zone "
               "violation or (3) imbalance between shards";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

} balancerCollectionStatusCmd;

}  // namespace
}  // namespace mongo

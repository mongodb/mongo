/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/auto_split_vector.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/request_types/auto_split_vector_gen.h"

namespace mongo {
namespace {

class AutoSplitVectorCommand final : public TypedCommand<AutoSplitVectorCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command returning the split points for a chunk, given the maximum chunk "
               "size.";
    }

    using Request = AutoSplitVectorRequest;
    using Response = AutoSplitVectorResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "The autoSplitVector command can only be invoked on shards (no CSRS).",
                    serverGlobalParams.clusterRole == ClusterRole::ShardServer);

            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            const auto& req = request();

            auto splitKeys = autoSplitVector(opCtx,
                                             ns(),
                                             req.getKeyPattern(),
                                             req.getMin(),
                                             req.getMax(),
                                             req.getMaxChunkSizeBytes());

            AutoSplitVectorResponse res;
            res.setSplitKeys(splitKeys);
            return res;
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::splitVector));
        }
    };
} autoSplitVectorCommand;

}  // namespace
}  // namespace mongo

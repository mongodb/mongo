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
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/configure_query_analyzer_cmd_gen.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace analyze_shard_key {

namespace {

/**
 * Returns a new command object with shard version and database version appended to it based on
 * the given routing info.
 */
BSONObj makeVersionedCmdObj(const CollectionRoutingInfo& cri,
                            const BSONObj& unversionedCmdObj,
                            ShardId shardId) {
    auto versionedCmdObj = appendShardVersion(unversionedCmdObj,
                                              cri.cm.isSharded() ? cri.getShardVersion(shardId)
                                                                 : ShardVersion::UNSHARDED());
    return appendDbVersionIfPresent(versionedCmdObj, cri.cm.dbVersion());
}

class ConfigureQueryAnalyzerCmd : public TypedCommand<ConfigureQueryAnalyzerCmd> {
public:
    using Request = ConfigureQueryAnalyzer;
    using Response = ConfigureQueryAnalyzerResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            const auto& nss = ns();
            uassertStatusOK(validateNamespace(nss));

            const auto cri = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
            const auto primaryShardId = cri.cm.dbPrimary();

            auto shard =
                uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, primaryShardId));
            auto versionedCmdObj = makeVersionedCmdObj(
                cri,
                CommandHelpers::filterCommandRequestForPassthrough(request().toBSON({})),
                primaryShardId);
            auto swResponse = shard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                DatabaseName::kAdmin.toString(),
                versionedCmdObj,
                Shard::RetryPolicy::kIdempotent);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(swResponse));

            auto response = ConfigureQueryAnalyzerResponse::parse(
                IDLParserContext("clusterConfigureQueryAnalyzer"), swResponse.getValue().response);
            return response;
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
                                                           ActionType::configureQueryAnalyzer));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Starts or stops collecting metrics about read and write queries against "
               "collection.";
    }
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(ConfigureQueryAnalyzerCmd,
                                       analyze_shard_key::gFeatureFlagAnalyzeShardKey);

}  // namespace

}  // namespace analyze_shard_key
}  // namespace mongo

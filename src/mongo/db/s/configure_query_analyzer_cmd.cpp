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
#include "mongo/db/db_raii.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/configure_query_analyzer_cmd_gen.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

void validateCommandOptions(OperationContext* opCtx,
                            const NamespaceString& nss,
                            QueryAnalyzerModeEnum mode,
                            boost::optional<double> sampleRate) {
    uassert(ErrorCodes::InvalidOptions,
            "Cannot specify 'sampleRate' when 'mode' is \"off\"",
            mode != QueryAnalyzerModeEnum::kOff || !sampleRate);

    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "'sampleRate' must be greater than 0",
            !sampleRate || (*sampleRate > 0));

    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        auto dbInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db()));

        ListCollections listCollections;
        listCollections.setDbName(nss.db());
        listCollections.setFilter(BSON("name" << nss.coll()));

        auto cmdResponse = executeCommandAgainstDatabasePrimary(
            opCtx,
            nss.db(),
            dbInfo,
            CommandHelpers::filterCommandRequestForPassthrough(listCollections.toBSON({})),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            Shard::RetryPolicy::kIdempotent);
        auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
        uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));

        auto firstBatch = remoteResponse.data.firstElement()["firstBatch"].Obj();
        BSONObjIterator it(firstBatch);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Cannot analyze queries for a non-existing collection",
                it.more());

        auto collection = it.next().Obj().getOwned();
        uassert(ErrorCodes::CommandNotSupportedOnView,
                "Cannot analyze queries for a view",
                collection.getStringField("type") != "view");

        uassert(6875000,
                str::stream() << "Found multiple collections with the same name '" << nss << "'",
                !it.more());
    } else {
        uassert(ErrorCodes::CommandNotSupportedOnView,
                "Cannot analyze queries for a view",
                !CollectionCatalog::get(opCtx)->lookupView(opCtx, nss));

        AutoGetCollectionForReadCommand collection(opCtx, nss);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Cannot analyze queries for a non-existing collection",
                collection);
    }
}

class ConfigureQueryAnalyzerCmd : public TypedCommand<ConfigureQueryAnalyzerCmd> {
public:
    using Request = ConfigureQueryAnalyzer;
    using Response = ConfigureQueryAnalyzerResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "configQueryAnalyzer command is not supported on a shardsvr mongod",
                    serverGlobalParams.clusterRole != ClusterRole::ShardServer);

            const auto& nss = ns();
            const auto mode = request().getMode();
            const auto sampleRate = request().getSampleRate();
            validateCommandOptions(opCtx, nss, mode, sampleRate);

            LOGV2(6875002,
                  "Configuring query analysis",
                  "nss"_attr = nss,
                  "mode"_attr = mode,
                  "sampleRate"_attr = sampleRate);

            return {};
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
                                                           ActionType::createCollection));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Starts or stops collecting metrics about read and write queries against a "
               "collection.";
    }
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(ConfigureQueryAnalyzerCmd,
                                       analyze_shard_key::gFeatureFlagAnalyzeShardKey);

}  // namespace

}  // namespace mongo

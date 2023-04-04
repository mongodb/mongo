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
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/configure_query_analyzer_cmd_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_shard_version_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace analyze_shard_key {

namespace {

constexpr int kMaxSampleRate = 1'000'000;

/*
 * The helper for 'validateCollectionOptions'. Performs the same validation as
 * 'validateCollectionOptionsLocally' but does that based on the listCollections response from the
 * primary shard for the database.
 */
StatusWith<UUID> validateCollectionOptionsOnPrimaryShard(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    ListCollections listCollections;
    listCollections.setDbName(nss.db());
    listCollections.setFilter(BSON("name" << nss.coll()));
    auto listCollectionsCmdObj =
        CommandHelpers::filterCommandRequestForPassthrough(listCollections.toBSON({}));

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    return shardVersionRetry(
        opCtx,
        catalogCache,
        nss,
        "validateCollectionOptionsOnPrimaryShard"_sd,
        [&]() -> StatusWith<UUID> {
            auto dbInfo = uassertStatusOK(catalogCache->getDatabaseWithRefresh(opCtx, nss.db()));
            auto cmdResponse = executeCommandAgainstDatabasePrimary(
                opCtx,
                nss.db(),
                dbInfo,
                listCollectionsCmdObj,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                Shard::RetryPolicy::kIdempotent);
            auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
            uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));

            auto cursorResponse =
                uassertStatusOK(CursorResponse::parseFromBSON(remoteResponse.data));
            auto firstBatch = cursorResponse.getBatch();

            if (firstBatch.empty()) {
                return Status{ErrorCodes::NamespaceNotFound,
                              str::stream() << "The namespace does not exist"};
            }
            uassert(6915300,
                    str::stream() << "The namespace corresponds to multiple collections",
                    firstBatch.size() == 1);

            auto listCollRepItem = ListCollectionsReplyItem::parse(
                IDLParserContext("ListCollectionsReplyItem"), firstBatch[0]);

            if (listCollRepItem.getType() == "view") {
                return Status{ErrorCodes::CommandNotSupportedOnView,
                              "The namespace corresponds to a view"};
            }
            if (auto obj = listCollRepItem.getOptions()) {
                auto options = uassertStatusOK(CollectionOptions::parse(*obj));
                if (options.encryptedFieldConfig.has_value()) {
                    return Status{ErrorCodes::IllegalOperation,
                                  str::stream()
                                      << "The collection has queryable encryption enabled"};
                }
            }

            auto info = listCollRepItem.getInfo();
            uassert(6915301,
                    str::stream() << "The listCollections reply for '" << nss
                                  << "' does not have the 'info' field",
                    info);
            return *info->getUuid();
        });
}

StatusWith<UUID> validateCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        return validateCollectionOptionsLocally(opCtx, nss);
    }
    return validateCollectionOptionsOnPrimaryShard(opCtx, nss);
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
                    "configQueryAnalyzer command is not supported on a standalone mongod",
                    repl::ReplicationCoordinator::get(opCtx)->isReplEnabled());
            uassert(ErrorCodes::IllegalOperation,
                    "configQueryAnalyzer command is not supported on a multitenant replica set",
                    !gMultitenancySupport);
            uassert(ErrorCodes::IllegalOperation,
                    "configQueryAnalyzer command is not supported on a shardsvr mongod",
                    !serverGlobalParams.clusterRole.exclusivelyHasShardRole());

            const auto& nss = ns();
            const auto mode = request().getMode();
            const auto sampleRate = request().getSampleRate();
            const auto newConfig = request().getConfiguration();

            uassertStatusOK(validateNamespace(nss));
            if (mode == QueryAnalyzerModeEnum::kOff) {
                uassert(ErrorCodes::InvalidOptions,
                        "Cannot specify 'sampleRate' when 'mode' is \"off\"",
                        !sampleRate);
            } else {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream()
                            << "'sampleRate' must be specified when 'mode' is not \"off\"",
                        sampleRate);
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'sampleRate' must be greater than 0",
                        *sampleRate > 0);
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'sampleRate' must be less than " << kMaxSampleRate,
                        *sampleRate < kMaxSampleRate);
            }
            auto collUuid = uassertStatusOK(validateCollectionOptions(opCtx, nss));

            QueryAnalyzerDocument qad;
            qad.setNs(nss);
            qad.setCollectionUuid(collUuid);
            qad.setConfiguration(newConfig);
            // TODO SERVER-69804: Implement start/stop timestamp in config.queryAnalyzers
            // document.
            LOGV2(6915001,
                  "Persisting query analyzer configuration",
                  logAttrs(nss),
                  "collectionUUID"_attr = collUuid,
                  "mode"_attr = mode,
                  "sampleRate"_attr = sampleRate);
            PersistentTaskStore<QueryAnalyzerDocument> store{
                NamespaceString::kConfigQueryAnalyzersNamespace};
            store.upsert(
                opCtx,
                BSON(QueryAnalyzerDocument::kCollectionUuidFieldName << qad.getCollectionUuid()),
                qad.toBSON(),
                WriteConcerns::kMajorityWriteConcernNoTimeout);

            Response response;
            // TODO SERVER-70019: Make configQueryAnalyzer return old configuration.
            response.setNewConfiguration(newConfig);
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

}  // namespace analyze_shard_key
}  // namespace mongo

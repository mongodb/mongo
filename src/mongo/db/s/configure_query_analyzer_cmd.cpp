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
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/configure_query_analyzer_cmd_gen.h"
#include "mongo/s/grid.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace analyze_shard_key {

namespace {

constexpr int kMaxSampleRate = 50;

/**
 * RAII type for the DDL lock. On a sharded cluster, the lock is the DDLLockManager collection lock.
 * On a replica set, the lock is the collection IX lock.
 */
class ScopedDDLLock {
    ScopedDDLLock(const ScopedDDLLock&) = delete;
    ScopedDDLLock& operator=(const ScopedDDLLock&) = delete;

public:
    static constexpr StringData lockReason{"configureQueryAnalyzer"_sd};

    ScopedDDLLock(OperationContext* opCtx, const NamespaceString& nss) {
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            ShardingDDLCoordinatorService::getService(opCtx)->waitForRecoveryCompletion(opCtx);
            auto ddlLockManager = DDLLockManager::get(opCtx);
            auto dbDDLLock = ddlLockManager->lock(
                opCtx, nss.db(), lockReason, DDLLockManager::kDefaultLockTimeout);

            // Check under the db lock if this is still the primary shard for the database.
            DatabaseShardingState::assertIsPrimaryShardForDb(opCtx, nss.dbName());

            _collDDLLock.emplace(ddlLockManager->lock(
                opCtx, nss.ns(), lockReason, DDLLockManager::kDefaultLockTimeout));
        } else {
            _autoColl.emplace(opCtx, nss, MODE_IX);
        }
    }

private:
    boost::optional<DDLLockManager::ScopedLock> _collDDLLock;
    boost::optional<AutoGetCollection> _autoColl;
};

/**
 * Waits for the system last opTime to be majority committed.
 */
void waitUntilMajorityLastOpTime(OperationContext* opCtx) {
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajority(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                           CancellationToken::uncancelable())
        .get();
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
                    "configQueryAnalyzer command is not supported on a configsvr mongod",
                    !serverGlobalParams.clusterRole.exclusivelyHasConfigRole());

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
                        str::stream()
                            << "'sampleRate' must be less than or equal to " << kMaxSampleRate,
                        (*sampleRate <= kMaxSampleRate) || TestingProctor::instance().isEnabled());
            }

            // Take the DDL lock to serialize this command with DDL commands.
            boost::optional<ScopedDDLLock> ddlLock;
            ddlLock.emplace(opCtx, nss);

            // Wait for the metadata for this collection in the CollectionCatalog to be majority
            // committed before validating its options and persisting the configuration.
            waitUntilMajorityLastOpTime(opCtx);
            auto collUuid = uassertStatusOK(validateCollectionOptions(opCtx, nss));

            LOGV2(6915001,
                  "Persisting query analyzer configuration",
                  logAttrs(nss),
                  "collectionUUID"_attr = collUuid,
                  "mode"_attr = mode,
                  "sampleRate"_attr = sampleRate);

            write_ops::FindAndModifyCommandRequest request(
                NamespaceString::kConfigQueryAnalyzersNamespace);

            using doc = QueryAnalyzerDocument;

            auto currentTime = opCtx->getServiceContext()->getFastClockSource()->now();
            if (mode == QueryAnalyzerModeEnum::kOff) {
                request.setUpsert(false);
                // If the mode is 'off', do not perform the update since that would overwrite the
                // existing stop time.
                request.setQuery(BSON(
                    doc::kNsFieldName
                    << nss.toString() << doc::kModeFieldName
                    << BSON("$ne" << QueryAnalyzerMode_serializer(QueryAnalyzerModeEnum::kOff))));

                std::vector<BSONObj> updates;
                updates.push_back(
                    BSON("$set" << BSON(doc::kModeFieldName
                                        << QueryAnalyzerMode_serializer(QueryAnalyzerModeEnum::kOff)
                                        << doc::kStopTimeFieldName << currentTime)));
                request.setUpdate(write_ops::UpdateModification(updates));
            } else {
                request.setUpsert(true);
                request.setQuery(BSON(doc::kNsFieldName << nss.toString()));

                std::vector<BSONObj> updates;
                BSONObjBuilder setBuilder;
                setBuilder.appendElements(BSON(doc::kCollectionUuidFieldName
                                               << collUuid << doc::kNsFieldName << nss.toString()));
                setBuilder.appendElements(newConfig.toBSON());
                // If the mode or collection UUID is different, set a new start time. Otherwise,
                // keep the original start time.
                setBuilder.append(
                    doc::kStartTimeFieldName,
                    BSON("$cond" << BSON(
                             "if" << BSON("$or" << BSON_ARRAY(
                                              BSON("$ne" << BSON_ARRAY(
                                                       ("$" + doc::kModeFieldName)
                                                       << QueryAnalyzerMode_serializer(mode)))
                                              << BSON("$ne" << BSON_ARRAY(
                                                          ("$" + doc::kCollectionUuidFieldName)
                                                          << collUuid))))
                                  << "then" << currentTime << "else"
                                  << ("$" + doc::kStartTimeFieldName))));
                updates.push_back(BSON("$set" << setBuilder.obj()));
                updates.push_back(BSON("$unset" << doc::kStopTimeFieldName));
                request.setUpdate(write_ops::UpdateModification(updates));
            }

            auto writeResult = [&] {
                if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
                    request.setWriteConcern(WriteConcerns::kMajorityWriteConcernNoTimeout.toBSON());

                    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
                    auto swResponse = configShard->runCommandWithFixedRetryAttempts(
                        opCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        DatabaseName::kConfig.toString(),
                        request.toBSON({}),
                        Shard::RetryPolicy::kIdempotent);
                    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(swResponse));
                    return write_ops::FindAndModifyCommandReply::parse(
                        IDLParserContext("configureQueryAnalyzer"), swResponse.getValue().response);
                }

                DBDirectClient client(opCtx);
                // It is illegal to wait for replication while holding a lock so instead wait below
                // after releasing the lock.
                request.setWriteConcern(BSONObj());
                return client.findAndModify(request);
            }();

            ddlLock.reset();
            if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
                // Wait for the write above to be majority committed.
                waitUntilMajorityLastOpTime(opCtx);
            }

            Response response;
            response.setNewConfiguration(newConfig);
            if (writeResult.getValue()) {
                auto preImageDoc =
                    doc::parse(IDLParserContext("configureQueryAnalyzer"), *writeResult.getValue());
                if (preImageDoc.getCollectionUuid() == collUuid) {
                    response.setOldConfiguration(preImageDoc.getConfiguration());
                }
            } else {
                uassert(ErrorCodes::IllegalOperation,
                        "Attempted to disable query sampling but query sampling was not active",
                        mode != QueryAnalyzerModeEnum::kOff);
            }

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

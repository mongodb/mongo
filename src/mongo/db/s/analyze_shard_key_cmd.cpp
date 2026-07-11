// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/analyze_shard_key_cmd_util.h"
#include "mongo/db/s/analyze_shard_key_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/analyze_shard_key_cmd_gen.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(analyzeShardKeyFailBeforeMetricsCalculation);

class AnalyzeShardKeyCmd : public TypedCommand<AnalyzeShardKeyCmd> {
public:
    using Request = AnalyzeShardKey;
    using Response = AnalyzeShardKeyResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "analyzeShardKey command is not supported on a standalone mongod",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());
            uassert(ErrorCodes::IllegalOperation,
                    "analyzeShardKey command is not supported on a multitenant replica set",
                    !gMultitenancySupport);

            uassert(ErrorCodes::InvalidOptions,
                    "Cannot skip analyzing all metrics",
                    request().getAnalyzeKeyCharacteristics() ||
                        request().getAnalyzeReadWriteDistribution());
            uassert(ErrorCodes::InvalidOptions,
                    "Cannot specify both 'sampleRate' and 'sampleSize'",
                    !request().getSampleRate() || !request().getSampleSize());

            const auto& nss = ns();

            AutoStatsTracker statsTracker(opCtx,
                                          nss,
                                          Top::LockType::ReadLocked,
                                          AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                          DatabaseProfileSettings::get(opCtx->getServiceContext())
                                              .getDatabaseProfileLevel(nss.dbName()));

            const auto& key = request().getKey();
            uassertStatusOK(validateNamespace(nss));
            const auto collUuid = uassertStatusOK(validateCollectionOptions(opCtx, nss));

            if (MONGO_unlikely(analyzeShardKeyFailBeforeMetricsCalculation.shouldFail())) {
                uasserted(
                    ErrorCodes::InternalError,
                    "Failing analyzeShardKey command before metrics calculation via a fail point");
            }

            const auto analyzeShardKeyId = UUID::gen();
            LOGV2(7790010,
                  "Start analyzing shard key",
                  logAttrs(nss),
                  "analyzeShardKeyId"_attr = analyzeShardKeyId,
                  "shardKey"_attr = key);

            Response response;

            // Calculate metrics about the characteristics of the shard key.
            if (request().getAnalyzeKeyCharacteristics()) {
                auto keyCharacteristics = analyze_shard_key::calculateKeyCharacteristicsMetrics(
                    opCtx,
                    analyzeShardKeyId,
                    nss,
                    collUuid,
                    key,
                    request().getSampleRate(),
                    request().getSampleSize());
                if (!keyCharacteristics) {
                    // No calculation was performed. By design this must be because the shard key
                    // does not have a supporting index. If the command is not requesting the
                    // metrics about the read and write distribution, there are no metrics to
                    // return to the user. So throw an error here.
                    uassert(
                        ErrorCodes::IllegalOperation,
                        "Cannot analyze the characteristics of a shard key that does not have a "
                        "supporting index",
                        request().getAnalyzeReadWriteDistribution());
                }
                response.setKeyCharacteristics(keyCharacteristics);
            }

            // Calculate metrics about the read and write distribution from sampled queries. Query
            // sampling is not supported on multitenant replica sets.
            if (request().getAnalyzeReadWriteDistribution()) {
                auto [readDistribution, writeDistribution] =
                    analyze_shard_key::calculateReadWriteDistributionMetrics(
                        opCtx, analyzeShardKeyId, nss, collUuid, key);
                response.setReadDistribution(readDistribution);
                response.setWriteDistribution(writeDistribution);
            }

            LOGV2(7790011,
                  "Finished analyzing shard key",
                  logAttrs(nss),
                  "analyzeShardKeyId"_attr = analyzeShardKeyId,
                  "shardKey"_attr = key);

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
                                                           ActionType::analyzeShardKey));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Returns metrics for evaluating a shard key for a collection.";
    }
};
MONGO_REGISTER_COMMAND(AnalyzeShardKeyCmd).forShard();

}  // namespace

}  // namespace analyze_shard_key
}  // namespace mongo

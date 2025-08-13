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
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/s/balancer_stats_registry.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/get_stats_for_balancing_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrGetStatsForBalancingCmd final : public TypedCommand<ShardsvrGetStatsForBalancingCmd> {
public:
    using Request = ShardsvrGetStatsForBalancing;
    using Reply = ShardsvrGetStatsForBalancingReply;

    // Default scale factor for data size (MiB)
    static constexpr int kDefaultScaleFactorMB{1024 * 1024};

    bool skipApiVersionCheck() const override {
        // Internal command (config -> shard).
        return true;
    }

    std::string help() const override {
        return "Internal command invoked by the config server to retrieve statistics from shard "
               "used for balancing";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::InvalidOptions,
                    "At least one collection must be specified",
                    request().getCollections().size());

            const auto scaleFactor = request().getScaleFactor().get_value_or(kDefaultScaleFactorMB);
            std::vector<CollStatsForBalancing> collStats;
            collStats.reserve(request().getCollections().size());

            for (const auto& nsWithOptUUID : request().getCollections()) {
                const auto collDataSizeScaled = static_cast<long long>(
                    _getCollDataSizeBytes(opCtx, nsWithOptUUID) / scaleFactor);
                collStats.emplace_back(nsWithOptUUID.getNs(), collDataSizeScaled);
            }
            return {std::move(collStats)};
        }

    private:
        long long _getCollDataSizeBytes(OperationContext* opCtx,
                                        const NamespaceWithOptionalUUID& nsWithOptUUID) const {
            const auto& ns = nsWithOptUUID.getNs();
            boost::optional<UUID> collUUID;
            long long numRecords{0};
            long long dataSizeBytes{0};

            {
                AutoStatsTracker statsTracker(
                    opCtx,
                    ns,
                    Top::LockType::ReadLocked,
                    AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                    DatabaseProfileSettings::get(opCtx->getServiceContext())
                        .getDatabaseProfileLevel(ns.dbName()));
                const auto collection =
                    acquireCollectionMaybeLockFree(opCtx,
                                                   CollectionAcquisitionRequest::fromOpCtx(
                                                       opCtx, ns, AcquisitionPrerequisites::kRead));
                if (collection.exists()) {
                    const auto& collectionPtr = collection.getCollectionPtr();
                    auto localCollUUID = collection.uuid();
                    if (auto wantedCollUUID = nsWithOptUUID.getUUID()) {
                        if (wantedCollUUID != localCollUUID) {
                            return 0LL;
                        }
                    }
                    collUUID.emplace(localCollUUID);
                    numRecords = collectionPtr->numRecords(opCtx);
                    dataSizeBytes = collectionPtr->dataSize(opCtx);
                }
            }

            // If the collection doesn't exists or is empty return 0
            if (numRecords == 0 || dataSizeBytes == 0) {
                return 0LL;
            }

            const long long numOrphanDocs =
                BalancerStatsRegistry::get(opCtx)->getCollNumOrphanDocs(*collUUID);

            if (numRecords <= numOrphanDocs) {
                // The number of records and the number of orphans documents are not updated
                // atomically, therefore it could totally happen that the total number of records is
                // less than the total number of orphans.
                return 0LL;
            }

            const auto avgObjSizeBytes = static_cast<long long>(dataSizeBytes / numRecords);
            return avgObjSizeBytes * (numRecords - numOrphanDocs);
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrGetStatsForBalancingCmd).forShard();

}  // namespace
}  // namespace mongo

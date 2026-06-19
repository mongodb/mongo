/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

/**
 * Counts the documents owned by 'nss' using a local '[{$count: "count"}]' aggregation. The count
 * runs at the operation context's read concern and shard version, so it reflects the caller's
 * desired read concern and excludes orphaned documents. When computing donor clone counts, the
 * read concern is expected to be a snapshot read with an 'atClusterTime' equal to the clone
 * timestamp.
 *
 * Throws a non-retryable error if no covering index hint can be chosen, so the coordinator skips
 * the count and the associated resharding verification rather than running an uncovered count.
 */
int64_t runCoveredCount(OperationContext* opCtx, const NamespaceString& nss) {
    auto coll = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead));
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << nss.toStringForErrorMsg() << " no longer exists",
            coll.exists());

    const auto& description = coll.getShardingDescription();
    boost::optional<BSONObj> shardKeyPattern;
    if (description.isSharded()) {
        shardKeyPattern = description.getKeyPattern();
    }

    auto hint =
        resharding::determineCloneCountHint(opCtx, coll.getCollectionPtr(), shardKeyPattern);
    uassert(10729002,
            str::stream() << "Cannot run a covered resharding donor clone count for "
                          << nss.toStringForErrorMsg()
                          << " because no covering index could be chosen",
            hint.has_value());

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$count" << "count"));
    AggregateCommandRequest aggRequest(nss, std::move(pipeline));
    aggRequest.setHint(*hint);

    // TODO SERVER-107180 always set rawData once 9.0 becomes last LTS.
    if (gFeatureFlagAllBinariesSupportRawDataOperations.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        aggRequest.setRawData(true);
    }

    DBDirectClient client(opCtx);
    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        &client, aggRequest, true /* secondaryOk */, false /* useExhaust */));

    // '$count' emits at most one document, and none when the collection is empty.
    if (!cursor->more()) {
        return 0;
    }
    auto doc = cursor->next();
    uassert(10729001,
            str::stream() << "Expected the resharding donor clone count aggregation to return a "
                             "document with a 'count' field but got "
                          << doc,
            doc.hasField("count"));
    return doc["count"].numberLong();
}

class ShardsvrReshardingDonorGetCloneCountCommand
    : public TypedCommand<ShardsvrReshardingDonorGetCloneCountCommand> {
public:
    using Request = ShardsvrReshardingDonorGetCloneCount;
    using Response = ShardsvrReshardingDonorGetCloneCountResponse;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command sent by the resharding coordinator to a donor shard to count the "
               "documents owned by this donor at the clone timestamp.";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrReshardingDonorGetCloneCount is only supported on shardsvr mongod",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
            uassert(
                ErrorCodes::InvalidOptions,
                str::stream() << "_shardsvrReshardingDonorGetCloneCount requires a snapshot read "
                                 "concern with an atClusterTime equal to the clone timestamp "
                              << request().getCloneTimestamp(),
                atClusterTime && atClusterTime->asTimestamp() == request().getCloneTimestamp());

            const auto& nss = request().getCommandParameter();
            auto count = runCoveredCount(opCtx, nss);
            LOGV2(11244401,
                  "Counted documents owned by donor shard at clone timestamp for post-cloning "
                  "verification",
                  "reshardingUUID"_attr = request().getReshardingUUID(),
                  logAttrs(nss),
                  "cloneTimestamp"_attr = request().getCloneTimestamp(),
                  "documentsToCopy"_attr = count);
            return Response{count};
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            // The coordinator must run this count at snapshot read concern with an 'atClusterTime'
            // equal to the clone timestamp. Any other read concern could observe a different point
            // in time and produce a count that does not match what the recipients cloned.
            static const Status kReadConcernNotSupported{
                ErrorCodes::InvalidOptions,
                "_shardsvrReshardingDonorGetCloneCount only supports snapshot read concern"};
            static const Status kDefaultReadConcernNotPermitted{
                ErrorCodes::InvalidOptions,
                "_shardsvrReshardingDonorGetCloneCount does not permit a default read concern"};
            return {
                {level != repl::ReadConcernLevel::kSnapshotReadConcern, kReadConcernNotSupported},
                kDefaultReadConcernNotPermitted};
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

MONGO_REGISTER_COMMAND(ShardsvrReshardingDonorGetCloneCountCommand)
    .requiresFeatureFlag(resharding::gFeatureFlagReshardingVerification)
    .forShard();

}  // namespace
}  // namespace mongo

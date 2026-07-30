// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/auto_split_vector.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/auto_split_vector_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

static constexpr int64_t kSmallestChunkSizeBytesSupported = 1024 * 1024;
static constexpr int64_t kBiggestChunkSizeBytesSupported = 1024 * 1024 * 1024;

std::string rangeString(const BSONObj& min, const BSONObj& max) {
    std::ostringstream os;
    os << "{min: " << min.toString() << " , max" << max.toString() << " }";
    return os.str();
}

class AutoSplitVectorCommand final : public TypedCommand<AutoSplitVectorCommand> {
public:
    bool skipApiVersionCheck() const override {
        /* Internal command (server to server) */
        return true;
    }

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
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto& req = request();

            uassert(ErrorCodes::ErrorCodes::InvalidOptions,
                    str::stream() << "maxChunksSizeBytes must lie within the range ["
                                  << kSmallestChunkSizeBytesSupported / (1024 * 1024) << "MB, "
                                  << kBiggestChunkSizeBytesSupported / (1024 * 1024) << "MB]",
                    req.getMaxChunkSizeBytes() >= kSmallestChunkSizeBytesSupported &&
                        req.getMaxChunkSizeBytes() <= kBiggestChunkSizeBytesSupported);

            const auto acquisition =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      opCtx, ns(), AcquisitionPrerequisites::kRead),
                                  MODE_IS);
            uassert(ErrorCodes::IllegalOperation,
                    fmt::format("{} is not supported on time-series collections",
                                Request::kCommandName),
                    !acquisition.exists() ||
                        !acquisition.getCollectionPtr()->isTimeseriesCollection());
            uassert(ErrorCodes::InvalidOptions,
                    fmt::format(
                        "The range {} for the namespace {} is required to be owned by one shard",
                        rangeString(req.getMin(), req.getMax()),
                        ns().toStringForErrorMsg()),
                    !acquisition.getShardingDescription().isSharded() ||
                        acquisition.getShardingFilter()->isRangeEntirelyOwned(
                            req.getMin(), req.getMax(), false /*includeMaxBound*/));


            auto [splitPoints, continuation] = autoSplitVector(opCtx,
                                                               acquisition,
                                                               req.getKeyPattern(),
                                                               req.getMin(),
                                                               req.getMax(),
                                                               req.getMaxChunkSizeBytes(),
                                                               req.getLimit());
            return Response(std::move(splitPoints), continuation);
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
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::splitVector));
        }
    };
};
MONGO_REGISTER_COMMAND(AutoSplitVectorCommand).forShard();

}  // namespace
}  // namespace mongo

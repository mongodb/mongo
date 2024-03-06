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


#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/auto_split_vector.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/auto_split_vector_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

using namespace fmt::literals;

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

            {
                const auto collection =
                    acquireCollection(opCtx,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx, ns(), AcquisitionPrerequisites::kRead),
                                      MODE_IS);
                uassert(
                    ErrorCodes::InvalidOptions,
                    "The range {} for the namespace {} is required to be owned by one shard"_format(
                        rangeString(req.getMin(), req.getMax()), ns().toStringForErrorMsg()),
                    !collection.getShardingDescription().isSharded() ||
                        collection.getShardingFilter()->isRangeEntirelyOwned(
                            req.getMin(), req.getMax(), false /*includeMaxBound*/));
            }

            auto [splitPoints, continuation] = autoSplitVector(opCtx,
                                                               ns(),
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

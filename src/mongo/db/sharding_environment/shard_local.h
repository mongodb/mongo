// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/rs_local_client.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ShardLocal : public Shard {
    ShardLocal(const ShardLocal&) = delete;
    ShardLocal& operator=(const ShardLocal&) = delete;

public:
    // ShardLocal doesn't have a "real" connection string since it's always a connection to the
    // local process, so it uses the hardcoded local connection string. Only used in tests.
    static const ConnectionString kLocalConnectionString;

    ShardLocal(const ShardHandle& handle,
               std::shared_ptr<ShardSharedStateCache::State> sharedState);

    ~ShardLocal() override = default;

    /**
     * These functions are implemented for the Shard interface's sake. They should not be called on
     * ShardLocal because doing so triggers invariants.
     */
    const ConnectionString& getConnString() const override;
    std::shared_ptr<RemoteCommandTargeter> getTargeter() const override;
    void updateReplSetMonitor(const HostAndPort& remoteHost,
                              const Status& remoteCommandStatus) override;

    std::string toString() const override;

    bool isRetriableError(const Status& status,
                          std::span<const std::string> errorLabels,
                          RetryPolicy options) const final;

    void runFireAndForgetCommand(OperationContext* opCtx,
                                 const ReadPreferenceSetting& readPref,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) override;

    BatchedCommandResponse runBatchWriteCommand(OperationContext* opCtx,
                                                Milliseconds maxTimeMS,
                                                const BatchedCommandRequest& batchRequest,
                                                const WriteConcernOptions& writeConcern,
                                                RetryPolicy retryPolicy) final;

private:
    StatusWith<Shard::CommandResponse> _runCommand(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& unused,
                                                   const TargetingMetadata& targetingMetadata,
                                                   const DatabaseName& dbName,
                                                   Milliseconds maxTimeMSOverrideUnused,
                                                   const BSONObj& cmdObj) final;

    RetryStrategy::Result<Shard::QueryResponse> _runExhaustiveCursorCommand(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const TargetingMetadata& targetingMetadata,
        const DatabaseName& dbName,
        Milliseconds maxTimeMSOverride,
        const BSONObj& cmdObj) final;

    RetryStrategy::Result<Shard::QueryResponse> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const TargetingMetadata& targetingMetadata,
        const repl::ReadConcernArgs& readConcern,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint = boost::none,
        const boost::optional<BSONObj>& projection = boost::none) final;

    RetryStrategy::Result<std::monostate> _runAggregation(
        OperationContext* opCtx,
        const TargetingMetadata& targetingMetadata,
        const AggregateCommandRequest& aggRequest,
        std::function<bool(const std::vector<BSONObj>& batch,
                           const boost::optional<BSONObj>& postBatchResumeToken)> callback) final;

    RSLocalClient _rsLocalClient;
};

}  // namespace mongo

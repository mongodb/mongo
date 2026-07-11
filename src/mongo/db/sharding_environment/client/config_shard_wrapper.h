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

/**
 * Wraps/decorates a Shard object (representing the config server) to attach extra config server
 * specific logic to the member functions in the Shard object.
 *
 * The ConfigShardWrapper wrapper should be used everytime you are specifically targeting a config
 * server (such as when running commands to modify catalog data). This wrapper is automtically
 * created when a config shard is retrieved through ShardRegistry::getConfigShard() and
 * ShardRegistry::createLocalConfigShard();
 */
class ConfigShardWrapper : public Shard {
    ConfigShardWrapper(const ConfigShardWrapper&) = delete;
    ConfigShardWrapper& operator=(const ConfigShardWrapper&) = delete;

public:
    ConfigShardWrapper(std::shared_ptr<Shard> configShard);

    ~ConfigShardWrapper() override = default;

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
                                 const BSONObj& cmdObj) final;

    BatchedCommandResponse runBatchWriteCommand(OperationContext* opCtx,
                                                Milliseconds maxTimeMS,
                                                const BatchedCommandRequest& batchRequest,
                                                const WriteConcernOptions& writeConcern,
                                                RetryPolicy retryPolicy) final;

private:
    StatusWith<Shard::CommandResponse> _runCommand(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& readPref,
                                                   const TargetingMetadata& targetingMetadata,
                                                   const DatabaseName& dbName,
                                                   Milliseconds maxTimeMSOverride,
                                                   const BSONObj& cmdObj) final;

    RetryStrategy::Result<Shard::QueryResponse> _runExhaustiveCursorCommand(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const TargetingMetadata& targetingMetadata,
        const DatabaseName& dbName,
        Milliseconds maxTimeMSOverride,
        const BSONObj& cmdObj) final;

    RetryStrategy::Result<QueryResponse> _exhaustiveFindOnConfig(
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

    ReadPreferenceSetting _attachConfigTimeToMinClusterTime(OperationContext* opCtx,
                                                            const ReadPreferenceSetting& readPref);

    const std::shared_ptr<Shard> _configShard;
};

}  // namespace mongo

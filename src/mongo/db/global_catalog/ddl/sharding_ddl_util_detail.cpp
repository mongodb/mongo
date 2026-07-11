// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"

namespace mongo::sharding_ddl_util_detail {

template std::vector<AsyncRequestsSender::Response>
sendAuthenticatedCommandToShards<write_ops::UpdateCommandRequest>(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<write_ops::UpdateCommandRequest>> originalOpts,
    const std::vector<ShardId>& shardIds,
    const boost::optional<std::vector<ShardVersion>>& shardVersions,
    ReadPreferenceSetting readPref,
    bool throwOnError);

template std::vector<AsyncRequestsSender::Response>
sendAuthenticatedCommandToShards<ShardsvrDropCollectionParticipant>(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<ShardsvrDropCollectionParticipant>> originalOpts,
    const std::vector<ShardId>& shardIds,
    const boost::optional<std::vector<ShardVersion>>& shardVersions,
    ReadPreferenceSetting readPref,
    bool throwOnError);

template std::vector<AsyncRequestsSender::Response>
sendAuthenticatedCommandToShards<ShardsvrCommitCreateDatabaseMetadata>(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<ShardsvrCommitCreateDatabaseMetadata>> originalOpts,
    const std::vector<ShardId>& shardIds,
    const boost::optional<std::vector<ShardVersion>>& shardVersions,
    ReadPreferenceSetting readPref,
    bool throwOnError);

template std::vector<AsyncRequestsSender::Response>
sendAuthenticatedCommandToShards<ShardsvrCommitDropDatabaseMetadata>(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<ShardsvrCommitDropDatabaseMetadata>> originalOpts,
    const std::vector<ShardId>& shardIds,
    const boost::optional<std::vector<ShardVersion>>& shardVersions,
    ReadPreferenceSetting readPref,
    bool throwOnError);

}  // namespace mongo::sharding_ddl_util_detail

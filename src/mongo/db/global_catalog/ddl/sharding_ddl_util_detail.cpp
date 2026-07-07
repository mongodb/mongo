/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

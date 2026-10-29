// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Simple struct for storing a write concern error with an endpoint.
 */
struct ShardWCError {
    ShardWCError(const ShardId& shardName, const WriteConcernErrorDetail& error)
        : shardName(shardName) {
        error.cloneTo(&this->error);
    }

    ShardWCError(const WriteConcernErrorDetail& error) {
        error.cloneTo(&this->error);
    }

    boost::optional<ShardId> shardName;
    WriteConcernErrorDetail error;
};

/**
 * Utility function to merge write concern errors received from various shards.
 */
boost::optional<WriteConcernErrorDetail> mergeWriteConcernErrors(
    const std::vector<ShardWCError>& wcErrors);

/*
 * Generate the write concern with which shard requests have to be internally sent.
 */
boost::optional<WriteConcernOptions> getWriteConcernForShardRequest(OperationContext* opCtx);

}  // namespace mongo

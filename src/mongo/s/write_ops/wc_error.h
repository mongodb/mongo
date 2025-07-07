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

#pragma once

#include "mongo/db/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/write_concern_error_detail.h"

namespace mongo {

/**
 * Simple struct for storing a write concern error with an endpoint.
 */
struct ShardWCError {
    ShardWCError(const ShardId& shardName, const WriteConcernErrorDetail& error)
        : shardName(shardName) {
        error.cloneTo(&this->error);
    }

    ShardId shardName;
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

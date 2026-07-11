// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/router_role/ns_targeter.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <map>
#include <set>

#include <boost/optional/optional.hpp>

namespace mongo {

class BatchWriteExecStats;
class OperationContext;

/**
 * The BatchWriteExec is able to execute client batch write requests, resulting in a batch
 * response to send back to the client.
 *
 * There are two main interfaces the exec uses to "run" the batch:
 *
 *  - the "targeter" used to generate child batch operations to send to particular shards
 *
 *  - the "dispatcher" used to send child batches to several shards at once, and retrieve the
 *    results
 *
 * Both the targeter and dispatcher are assumed to be dedicated to this particular
 * BatchWriteExec instance.
 *
 */
class BatchWriteExec {
public:
    /**
     * Executes a client batch write request by sending child batches to several shard
     * endpoints, and returns a client batch write response.
     *
     * This function does not throw, any errors are reported via the clientResponse.
     */
    static void executeBatch(OperationContext* opCtx,
                             NSTargeter& targeter,
                             const BatchedCommandRequest& clientRequest,
                             BatchedCommandResponse* clientResponse,
                             BatchWriteExecStats* stats);
};

struct HostOpTime {
    HostOpTime(repl::OpTime ot, OID e) : opTime(ot), electionId(e) {};
    HostOpTime() {};
    repl::OpTime opTime;
    OID electionId;
};

typedef std::map<ConnectionString, HostOpTime> HostOpTimeMap;

// TODO SERVER-109104 This macro will be resolved once we delete the BatchWriteExec.
class [[MONGO_MOD_NEEDS_REPLACEMENT]] BatchWriteExecStats {
public:
    BatchWriteExecStats() : numRounds(0), numStaleShardBatches(0), numStaleDbBatches(0) {}

    void noteTargetedShard(const ShardId& shardId);
    void noteNumShardsOwningChunks(int nShardsOwningChunks);
    void noteTargetedCollectionIsSharded(bool isSharded);

    const std::set<ShardId>& getTargetedShards() const;
    boost::optional<int> getNumShardsOwningChunks() const;
    bool hasTargetedShardedCollection() const;

    /**
     * Set of methods to determine whether this 'BatchWriteExecStats' object should be ignored or
     * not (that is, whether it should not be used to update targeting or query counter stats).
     */
    void markIgnore() {
        _ignore = true;
    }
    bool getIgnore() const {
        return _ignore;
    }

    // Expose via helpers if this gets more complex

    // Number of round trips required for the batch
    int numRounds;
    // Number of stale batches due to "retargeting needed" errors
    int numStaleShardBatches;
    // Number of stale batches due to StaleDbVersion
    int numStaleDbBatches;

private:
    std::set<ShardId> _targetedShards;
    boost::optional<int> _numShardsOwningChunks;
    bool _hasTargetedShardedCollection = false;
    bool _ignore = false;
};

void updateHostsTargetedMetrics(OperationContext* opCtx,
                                BatchedCommandRequest::BatchType batchType,
                                int nShardsOwningChunks,
                                int nShardsTargeted,
                                bool isSharded);

}  // namespace mongo

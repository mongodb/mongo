/**
 * Concurrently runs movePrimary operations while toggling the replica set write block on random
 * shards.
 *
 * When movePrimary clones non-empty collections onto a recipient whose replica set writes are blocked,
 * the cloner insert is rejected with ReplicaSetWritesBlocked and the operation is aborted and rolled
 * back (the database primary is left unchanged and the partially cloned data on the recipient is
 * dropped). Cloning only empty collections, by contrast, performs no inserts and still succeeds.
 * This workload exercises that interaction under concurrency.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_persistence,
 *   featureFlagBlockReplicaSetWrites,
 * ]
 */

import {ShardingTopologyHelpers} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/sharding_topology_helpers.js";
import {
    disableReplicaSetWriteBlock,
    enableReplicaSetWriteBlock,
} from "jstests/libs/block_replica_set_writes_utils.js";

const kWriteBlockReason = "InsufficientDiskSpace";

// Use the admin DB so writes bypass the replica set write block (isOnInternalDb() check in
// replica_set_write_block_state.cpp).
const logExceptionsDBName = "admin";
const logExceptionsCollName = "exceptions_log";

function logException(db, exceptionCode) {
    db = db.getSiblingDB(logExceptionsDBName);
    const coll = db[logExceptionsCollName];
    assert.commandWorked(coll.insert({code: exceptionCode}));
}

function checkExceptionHasBeenThrown(db, exceptionCode) {
    db = db.getSiblingDB(logExceptionsDBName);
    const coll = db[logExceptionsCollName];
    const count = coll.countDocuments({code: exceptionCode});
    const errorName = Object.prototype.hasOwnProperty.call(ErrorCodeStrings, exceptionCode)
        ? ErrorCodeStrings[exceptionCode]
        : exceptionCode;
    assert.gte(count, 1, "No exception has been thrown", {errorName, exceptionCode});
    jsTest.log.info("Thrown exceptions", {count, errorName, exceptionCode});
}

/**
 * Returns a random shard on which the replica set write block may be toggled, excluding the config
 * shard.
 */
function randomBlockableShard(shardInfo) {
    const shardNames = Object.keys(shardInfo.rsConns).filter((shardName) => shardName !== "config");
    return shardNames[Random.randInt(shardNames.length)];
}

export const $config = (function () {
    const data = {
        // Prefix for the unsharded, non-empty collections created in setup(). movePrimary clones
        // these onto the recipient, which is the path that the replica set write block rejects.
        collPrefix: "rs_write_block_coll_",
        numDocsPerColl: 50,
    };

    const states = {
        init: function init(db, collName, connCache) {
            // No per-thread initialization is required: the data to clone is created once in setup().
        },

        movePrimary: function movePrimary(db, collName, connCache) {
            const shards = ShardingTopologyHelpers.getShardNames(db);
            const toShard = shards[Random.randInt(shards.length)];
            jsTest.log.info("Running movePrimary", {db: db.getName(), to: toShard});

            const expectedExceptions = [
                // The recipient has replica set writes blocked, so the cloner insert was rejected and
                // the movePrimary was aborted and rolled back.
                ErrorCodes.ReplicaSetWritesBlocked,
                // Concurrent movePrimary operation on the same database but a different destination shard.
                ErrorCodes.ConflictingOperationInProgress,
                // The non-idempotent clone phase failed after the recipient already started cloning,
                // so the movePrimary was aborted (orphaned data on the recipient has been removed).
                7120202,
            ];
            if (TestData.shardsAddedRemoved) {
                expectedExceptions.push(ErrorCodes.ShardNotFound);
            }
            const res = db.adminCommand({movePrimary: db.getName(), to: toShard});
            assert.commandWorkedOrFailedWithCode(res, expectedExceptions);
            if (!res.ok && res.code === ErrorCodes.ReplicaSetWritesBlocked) {
                logException(db, ErrorCodes.ReplicaSetWritesBlocked);
            }
        },

        enableWriteBlock: function enableWriteBlock(db, collName, connCache) {
            ShardingTopologyHelpers.executeWithShardInfo(db, this.tid, (shardInfo) => {
                const shard = randomBlockableShard(shardInfo);
                jsTest.log.info("Enabling replica set write block", {shard});
                enableReplicaSetWriteBlock(
                    shardInfo.rsConns[shard].getDB("admin"),
                    true,
                    kWriteBlockReason,
                );
            });
        },

        disableWriteBlock: function disableWriteBlock(db, collName, connCache) {
            ShardingTopologyHelpers.executeWithShardInfo(db, this.tid, (shardInfo) => {
                const shard = randomBlockableShard(shardInfo);
                jsTest.log.info("Disabling replica set write block", {shard});
                // Disabling a shard that was never blocked is a no-op.
                disableReplicaSetWriteBlock(
                    shardInfo.rsConns[shard].getDB("admin"),
                    kWriteBlockReason,
                );
            });
        },
    };

    const standardTransition = {
        movePrimary: 0.4,
        enableWriteBlock: 0.3,
        disableWriteBlock: 0.3,
    };

    const transitions = {
        init: standardTransition,
        movePrimary: standardTransition,
        enableWriteBlock: standardTransition,
        disableWriteBlock: standardTransition,
    };

    function setup(db, collName, cluster) {
        // Drop any leftover exception log from a prior workload run. CleanupConcurrencyWorkloads
        // skips the admin database, so without this the teardown assertion can be satisfied by
        // entries written during an earlier test in the same suite.
        cluster.getConfigPrimaryNode().getDB(logExceptionsDBName)[logExceptionsCollName].drop();

        // Populate one unsharded collection per thread with data, single-threaded and before any
        // write block is enabled, so that subsequent movePrimary operations have user data to clone
        // onto the recipient. Using multiple collections also exercises the recipient cleanup path,
        // which must drop every partially cloned collection when the movePrimary aborts.
        for (let tid = 0; tid < this.threadCount; ++tid) {
            const docs = [];
            for (let i = 0; i < this.numDocsPerColl; ++i) {
                docs.push({_id: i, x: i});
            }
            assert.commandWorked(db[`${this.collPrefix}${tid}`].insert(docs));
        }
    }

    function teardown(db, collName, cluster) {
        // Disable the replica set write block on every shard so no shard is left blocked for the
        // suite's post-workload hooks (e.g. CleanupConcurrencyWorkloads, which drops the databases).
        const shardInfo = ShardingTopologyHelpers.getShardInfo(db, 0);
        for (const shard of Object.keys(shardInfo.rsConns)) {
            if (shard === "config") {
                continue;
            }
            disableReplicaSetWriteBlock(shardInfo.rsConns[shard].getDB("admin"), kWriteBlockReason);
        }

        // Verify that movePrimary was actually rejected by a write-blocked recipient at least once:
        // with sufficient concurrency and iterations, a ReplicaSetWritesBlocked error should always
        // occur, so its absence likely indicates a bug.
        checkExceptionHasBeenThrown(db, ErrorCodes.ReplicaSetWritesBlocked);
        cluster.getConfigPrimaryNode().getDB(logExceptionsDBName)[logExceptionsCollName].drop();
    }

    return {
        threadCount: 5,
        iterations: 30,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true,
    };
})();

/**
 * Concurrently runs movePrimary and index build DDL operations while toggling the replica set write
 * block on random shards.
 *
 * When movePrimary clones non-empty collections onto a recipient whose replica set writes are blocked,
 * the cloner insert is rejected with ReplicaSetWritesBlocked and the operation is aborted and rolled
 * back (the database primary is left unchanged and the partially cloned data on the recipient is
 * dropped). Cloning only empty collections, by contrast, performs no inserts and still succeeds.
 * Index builds on user collections are also blocked when a replica set write block is active.
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
    checkExceptionHasBeenThrown,
    getRandomCollName,
    getRandomDbName,
} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/random_ddl_utils.js";
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
        dbNames: ["rs_write_block_db_0", "rs_write_block_db_1", "rs_write_block_db_2"],
        collNames: ["coll_a", "coll_b", "coll_c"],
        numDocsPerColl: 50,
    };

    const states = {
        init: function init(db, collName, connCache) {
            // No per-thread initialization is required: the data to clone is created once in setup().
        },

        movePrimary: function movePrimary(db, collName, connCache) {
            const dbName = getRandomDbName(this.threadCount, this.dbNames);
            const shards = ShardingTopologyHelpers.getShardNames(db);
            const toShard = shards[Random.randInt(shards.length)];
            jsTest.log.info("Running movePrimary", {db: dbName, to: toShard});

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
            const res = db.adminCommand({movePrimary: dbName, to: toShard});
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

        createIndex: function createIndex(db, collName, connCache) {
            const targetDb = db.getSiblingDB(getRandomDbName(this.threadCount, this.dbNames));
            const coll = targetDb[getRandomCollName(this.threadCount, this.collNames)];
            jsTest.log.info("Running createIndex", {coll: coll.getFullName()});
            assert.commandWorkedOrFailedWithCode(coll.createIndex({x: 1}), [
                // The index build started before the write block was enabled and then got aborted.
                ErrorCodes.IndexBuildAborted,
                // The shard holding the collection has index builds blocked.
                ErrorCodes.ReplicaSetWritesBlocked,
                // A concurrent movePrimary is cloning this collection.
                ErrorCodes.MovePrimaryInProgress,
                // A concurrent thread already built or is building the same index on this collection.
                ErrorCodes.IndexBuildAlreadyInProgress,
            ]);
        },

        dropIndex: function dropIndex(db, collName, connCache) {
            const targetDb = db.getSiblingDB(getRandomDbName(this.threadCount, this.dbNames));
            const coll = targetDb[getRandomCollName(this.threadCount, this.collNames)];
            jsTest.log.info("Running dropIndex", {coll: coll.getFullName()});
            assert.commandWorkedOrFailedWithCode(coll.dropIndex({x: 1}), [
                // The index may not exist if createIndex never succeeded or ran concurrently.
                ErrorCodes.IndexNotFound,
                // The shard holding the collection has index builds blocked.
                ErrorCodes.ReplicaSetWritesBlocked,
                // A concurrent movePrimary is cloning this collection.
                ErrorCodes.MovePrimaryInProgress,
            ]);
        },
    };

    const standardTransition = {
        movePrimary: 0.35,
        enableWriteBlock: 0.2,
        disableWriteBlock: 0.2,
        createIndex: 0.15,
        dropIndex: 0.1,
    };

    const transitions = {
        init: standardTransition,
        movePrimary: standardTransition,
        enableWriteBlock: standardTransition,
        disableWriteBlock: standardTransition,
        createIndex: standardTransition,
        dropIndex: standardTransition,
    };

    function setup(db, collName, cluster) {
        // Drop any leftover exception log from a prior workload run. CleanupConcurrencyWorkloads
        // skips the admin database, so without this the teardown assertion can be satisfied by
        // entries written during an earlier test in the same suite.
        cluster.getConfigPrimaryNode().getDB(logExceptionsDBName)[logExceptionsCollName].drop();

        // Populate all db/collection combinations with data, single-threaded and before any write
        // block is enabled, so that subsequent movePrimary operations have user data to clone onto
        // the recipient. Using multiple databases reduces serialization across concurrent movePrimary
        // calls. Using multiple collections per database also exercises the recipient cleanup path,
        // which must drop every partially cloned collection when the movePrimary aborts.
        const docs = [];
        for (let i = 0; i < this.numDocsPerColl; ++i) {
            docs.push({_id: i, x: i});
        }
        for (const dbName of this.dbNames) {
            for (const coll of this.collNames) {
                assert.commandWorked(db.getSiblingDB(dbName)[coll].insert(docs));
            }
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
        checkExceptionHasBeenThrown(
            db,
            ErrorCodes.ReplicaSetWritesBlocked,
            logExceptionsDBName,
            logExceptionsCollName,
        );
        cluster.getConfigPrimaryNode().getDB(logExceptionsDBName)[logExceptionsCollName].drop();
    }

    return {
        threadCount: 10,
        iterations: 60,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true,
    };
})();

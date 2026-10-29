/**
 * Concurrently runs movePrimary, resharding, and index build DDL operations while toggling the
 * replica set write block on random shards.
 *
 * When movePrimary clones non-empty collections onto a recipient whose replica set writes are blocked,
 * the cloner insert is rejected with ReplicaSetWritesBlocked and the operation is aborted and rolled
 * back (the database primary is left unchanged and the partially cloned data on the recipient is
 * dropped). Cloning only empty collections, by contrast, performs no inserts and still succeeds.
 * Index builds on user collections are also blocked when a replica set write block is active.
 *
 * For an already-running resharding operation, a recipient treats ReplicaSetWritesBlocked
 * encountered while cloning or applying oplog entries as a transient error: it pauses and keeps
 * retrying until the write block is disabled, instead of aborting the operation. However, starting
 * a brand-new resharding operation with a recipient whose writes are already blocked is rejected
 * synchronously with ReplicaSetWritesBlocked.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_persistence,
 *   featureFlagBlockReplicaSetWrites,
 *   does_not_support_transactions,
 *   catches_command_failures,
 * ]
 */

import {ShardingTopologyHelpers} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/sharding_topology_helpers.js";
import {
    getRandomCollName,
    getRandomDbName,
    runWriteBlockToggler,
} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/random_ddl_utils.js";
import {disableReplicaSetWriteBlock} from "jstests/libs/block_replica_set_writes_utils.js";
import {Thread} from "jstests/libs/parallelTester.js";

const kWriteBlockReason = "InsufficientDiskSpace";

// Bounds how long a single reshardCollection call may hold/retry on ReplicaSetWritesBlocked.
const kReshardCollectionMaxTimeMS = 0.8 * 1000;

// Bounds how long a single write block may be enabled on a shard.
const kMaxWriteBlockTimeMS = 1 * 1000;

// How long writes are left unblocked between consecutive blocks. Must be comfortably longer than kClientRetryBackoffMS
// so a retry still lands in the window.
const kWriteBlockUnblockedTimeMS = 4 * 1000;

// Pins the retry backoff  to a small fixed delay,
// bypassing the default exponential-with-jitter backoff that saturates at 10s. It allows to reliably
// catch the unblocked window the toggler leaves between blocks instead of backing off past it.
const kClientRetryBackoffMS = 50;

let writeBlockToggler;
let writeBlockTogglerStopLatch;

/**
 * Disables the replica set write block on every shard.
 */
function disableAllWriteBlocks(shardInfo) {
    for (const shard of Object.keys(shardInfo.rsConns)) {
        if (shard === "config") {
            continue;
        }
        disableReplicaSetWriteBlock(shardInfo.rsConns[shard].getDB("admin"), kWriteBlockReason);
    }
}

export const $config = (function () {
    const data = {
        dbNames: ["rs_write_block_db_0", "rs_write_block_db_1", "rs_write_block_db_2"],
        collNames: ["coll_a", "coll_b", "coll_c"],
        // Empty collections created in every movePrimary database. Cloning an empty collection
        // performs no inserts, so movePrimary onto a write-blocked recipient still succeeds for them,
        // exercising the empty-clone path alongside the non-empty collNames above.
        emptyCollNames: ["empty_coll_a", "empty_coll_b"],
        // Small number of docs per movePrimary collection: enough for the cloner to attempt an insert
        // (so a write-blocked recipient rejects it with ReplicaSetWritesBlocked) while keeping the
        // clone fast, so movePrimary does not hold these databases long enough to starve concurrent
        // DDL operations.
        numMovePrimaryDocsPerColl: 10000,

        // Separate databases that createIndex targets. movePrimary never runs against these, so index
        // builds are not rejected with MovePrimaryInProgress. Their collections are sized large
        // (numIndexBuildDocsPerColl) so a createIndex build stays in progress long enough to still be
        // running when a write block is enabled on the same shard, so the build is aborted with
        // IndexBuildAborted ("Write blocking") rather than finishing before the toggle lands.
        indexBuildDbNames: [
            "rs_write_block_index_db_0",
            "rs_write_block_index_db_1",
            "rs_write_block_index_db_2",
        ],
        indexBuildCollNames: ["index_coll_a", "index_coll_b", "index_coll_c", "index_coll_d"],
        numIndexBuildDocsPerColl: 20000,
        numReshardDocsPerColl: 20000,
        // Resharding uses its own databases because reshardCollection holds an IX DDL lock on its
        // database while movePrimary needs an X DDL lock. Separate collections alone do not prevent
        // the two operations from contending at the database-lock level.
        reshardDbNames: [
            "rs_write_block_reshard_db_0",
            "rs_write_block_reshard_db_1",
            "rs_write_block_reshard_db_2",
        ],
        reshardCollNames: ["reshard_coll_a", "reshard_coll_b", "reshard_coll_c"],
        reshardKeyFields: ["shard_key_a", "shard_key_b"],
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
        },

        resharding: function resharding(db, collName, connCache) {
            const dbName = getRandomDbName(this.threadCount, this.reshardDbNames);
            const coll = getRandomCollName(this.threadCount, this.reshardCollNames);
            const fullNs = db.getSiblingDB(dbName)[coll].getFullName();
            const newKeyField = this.reshardKeyFields[Random.randInt(this.reshardKeyFields.length)];
            jsTest.log.info("Running reshardCollection", {ns: fullNs, key: newKeyField});

            const expectedExceptions = [
                // A concurrent resharding operation on the same collection is already in progress.
                ErrorCodes.ConflictingOperationInProgress,
                ErrorCodes.ReshardCollectionInProgress,
                // A recipient already has replica set writes blocked at the time this new resharding
                // operation is starting, so it is rejected synchronously at the recipient start gate.
                ErrorCodes.ReplicaSetWritesBlocked,
                // A concurrent resharding operation is being aborted but its recipient state
                // document hasn't been removed yet: the recipient's completion future can resolve
                // before its on-disk state document is deleted.
                5563803,
            ];
            if (TestData.shardsAddedRemoved) {
                expectedExceptions.push(ErrorCodes.ShardNotFound);
            }

            const reshardCmd = {
                reshardCollection: fullNs,
                key: {[newKeyField]: 1},
                numInitialChunks: 1 + Random.randInt(4),
                demoMode: true,
            };
            const res = db.adminCommand(
                Object.assign({}, reshardCmd, {maxTimeMS: kReshardCollectionMaxTimeMS}),
            );
            if (!res.ok && res.code === ErrorCodes.MaxTimeMSExpired) {
                // The client timed out while an in-flight resharding recipient was paused by the
                // write block. The background toggler bounds each block to one second, so the
                // coordinator will make progress without requiring an FSM worker to unblock it.
                return;
            }
            assert.commandWorkedOrFailedWithCode(res, expectedExceptions);
            if (!res.ok && res.code === ErrorCodes.ReplicaSetWritesBlocked) {
                // Only the synchronous start-time rejection is expected to ever reach the caller.
                // A ReplicaSetWritesBlocked surfacing from the generic write path would mean the
                // transient-error retry logic failed to hold.
                assert(
                    res.errmsg.startsWith(
                        "Resharding blocked because replica set writes are blocked",
                    ),
                    "ReplicaSetWritesBlocked leaked from an in-flight resharding operation instead " +
                        "of being retried internally",
                    {res},
                );
            }
        },

        createIndex: function createIndex(db, collName, connCache) {
            const targetDb = db.getSiblingDB(
                getRandomDbName(this.threadCount, this.indexBuildDbNames),
            );
            const coll = targetDb[getRandomCollName(this.threadCount, this.indexBuildCollNames)];
            jsTest.log.info("Running createIndex", {coll: coll.getFullName()});
            const res = coll.createIndex({x: 1});
            assert.commandWorkedOrFailedWithCode(res, [
                // The index build started before the write block was enabled and then got aborted.
                ErrorCodes.IndexBuildAborted,
                // The shard holding the collection has index builds blocked.
                ErrorCodes.ReplicaSetWritesBlocked,
                // A concurrent thread already built or is building the same index on this collection.
                ErrorCodes.IndexBuildAlreadyInProgress,
            ]);

            // Drop the index right away so the next createIndex call on this collection has a real
            // build to do, instead of becoming a no-op once the index already exists.
            if (res.ok) {
                assert.commandWorkedOrFailedWithCode(coll.dropIndex({x: 1}), [
                    // A concurrent thread already dropped it, or the build above never completed.
                    ErrorCodes.IndexNotFound,
                ]);
            }
        },
    };

    const standardTransition = {
        movePrimary: 0.3,
        resharding: 0.35,
        createIndex: 0.35,
    };

    const transitions = {
        init: standardTransition,
        movePrimary: standardTransition,
        resharding: standardTransition,
        createIndex: standardTransition,
    };

    function setup(db, collName, cluster) {
        // Pin the resharding participants' retry backoff to a small fixed delay so a recipient paused
        // on ReplicaSetWritesBlocked retries promptly and catches the unblocked window the toggler
        // leaves between blocks (kWriteBlockUnblockedTimeMS).
        cluster.executeOnMongodNodes((nodeAdminDB) => {
            assert.commandWorked(
                nodeAdminDB.adminCommand({
                    configureFailPoint: "setBackoffDelayForTesting",
                    mode: "alwaysOn",
                    data: {backoffDelayMs: kClientRetryBackoffMS},
                }),
            );
        });

        // Populate the movePrimary databases with a small amount of data, single-threaded and before
        // any write block is enabled, so that subsequent movePrimary operations have user data to
        // clone onto the recipient. Using multiple databases reduces serialization across concurrent
        // movePrimary calls. Using multiple collections per database also exercises the recipient
        // cleanup path, which must drop every partially cloned collection when the movePrimary aborts.
        const movePrimaryDocs = [];
        for (let i = 0; i < this.numMovePrimaryDocsPerColl; ++i) {
            movePrimaryDocs.push({_id: i, x: i});
        }
        for (const dbName of this.dbNames) {
            const targetDb = db.getSiblingDB(dbName);
            for (const coll of this.collNames) {
                assert.commandWorked(targetDb[coll].insert(movePrimaryDocs));
            }
            // Create the empty collections without inserting any documents, so movePrimary exercises
            // the empty-clone path that succeeds even on a write-blocked recipient.
            for (const coll of this.emptyCollNames) {
                assert.commandWorked(targetDb.createCollection(coll));
            }
        }

        // Populate the index-build databases with a large amount of data so a createIndex build on
        // them stays in progress long enough to be caught by a write block. movePrimary never targets
        // these databases, so these builds are not rejected with MovePrimaryInProgress.
        // Pin each index-build database to a distinct blockable shard round-robin so builds are spread
        // across all shards and any shard's write block can catch an in-flight build.
        const blockableShards = ShardingTopologyHelpers.getShardNames(db).filter(
            (shard) => shard !== "config",
        );
        const indexBuildDocs = [];
        for (let i = 0; i < this.numIndexBuildDocsPerColl; ++i) {
            indexBuildDocs.push({_id: i, x: i});
        }
        this.indexBuildDbNames.forEach((dbName, i) => {
            const targetDb = db.getSiblingDB(dbName);
            // Create the database (via a first collection) and pin its primary before inserting, so
            // the movePrimary has no data to clone.
            assert.commandWorked(targetDb.createCollection(this.indexBuildCollNames[0]));
            assert.commandWorked(
                db.adminCommand({
                    movePrimary: dbName,
                    to: blockableShards[i % blockableShards.length],
                }),
            );
            for (const coll of this.indexBuildCollNames) {
                assert.commandWorked(targetDb[coll].insert(indexBuildDocs));
            }
        });

        // Shard the resharding collections up front and index both candidate shard key fields, so
        // reshardCollection always has a valid alternate key to target.
        const reshardDocs = [];
        for (let i = 0; i < this.numReshardDocsPerColl; ++i) {
            reshardDocs.push({
                _id: i,
                [this.reshardKeyFields[0]]: i,
                [this.reshardKeyFields[1]]: i,
            });
        }
        for (const dbName of this.reshardDbNames) {
            const targetDb = db.getSiblingDB(dbName);
            for (const coll of this.reshardCollNames) {
                const namespace = targetDb[coll].getFullName();
                assert.commandWorked(
                    targetDb.runCommand({
                        createIndexes: coll,
                        indexes: this.reshardKeyFields.map((field) => ({
                            key: {[field]: 1},
                            name: `${field}_1`,
                        })),
                        writeConcern: {w: "majority"},
                    }),
                );
                assert.commandWorked(
                    targetDb.adminCommand({
                        shardCollection: namespace,
                        key: {[this.reshardKeyFields[0]]: 1},
                    }),
                );
                assert.commandWorked(targetDb[coll].insert(reshardDocs));
            }
        }

        // Start the write block toggler
        const blockableShardConnections = assert
            .commandWorked(db.adminCommand({listShards: 1}))
            .shards.filter((shard) => shard._id !== "config")
            .map((shard) => ({name: shard._id, host: shard.host}));
        writeBlockTogglerStopLatch = new CountDownLatch(1);
        writeBlockToggler = new Thread(
            runWriteBlockToggler,
            blockableShardConnections,
            writeBlockTogglerStopLatch,
            kWriteBlockReason,
            kMaxWriteBlockTimeMS,
            kWriteBlockUnblockedTimeMS,
            Random.randInt(1e13),
            true /* allowDeletions */,
        );
        writeBlockToggler.start();
    }

    function teardown(db, collName, cluster) {
        // Stop the write block toggler
        writeBlockTogglerStopLatch.countDown();
        writeBlockToggler.join();

        // Disable the replica set write block on every shard so no shard is left blocked for the
        // suite's post-workload hooks (e.g. CleanupConcurrencyWorkloads, which drops the databases).
        disableAllWriteBlocks(ShardingTopologyHelpers.getShardInfo(db, 0));

        // Clear the retry-backoff failpoint so it does not leak into later tests.
        cluster.executeOnMongodNodes((nodeAdminDB) => {
            assert.commandWorked(
                nodeAdminDB.adminCommand({
                    configureFailPoint: "setBackoffDelayForTesting",
                    mode: "off",
                }),
            );
        });

        // We intentionally do not assert that specific exceptions (e.g. ReplicaSetWritesBlocked,
        // IndexBuildAborted, MaxTimeMSExpired) were thrown during the workload: it is hard to
        // guarantee that every test run gets at least one of each exception, so such checks would
        // be flaky.
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

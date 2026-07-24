/**
 * Concurrently runs a wide range of sharding DDL operations while a mix of reader/writer threads
 * drive CRUD against the same collections and a single thread periodically stops writes.
 *
 * The workload is iteration-bounded: mixed workers each make kNumWorkerIterations selections from
 * the CRUD/DDL distribution while a separately spawned thread continuously toggles the write block.
 *
 * Every FSM thread is an identical mixed worker. Each owns one sharded collection
 *    plus a small fixed set of scratch/capped resources, and selects CRUD 70% of
 *    the time and DDL 30% of the time. DDLs act only on that worker's resources (so that they do not conflict each other), while CRUD targets
 *    a random worker's collection so the two operation classes race continuously. movePrimary runs
 *    only on a dedicated per-thread database that never hosts reshardCollection or cross-database
 *    rename, so its DB DDL X cannot convoy behind those ops' long-lived DB DDL IX on the main FSM
 *    database.
 *
 * DDL operations are split into two groups by how they should interact with the write block:
 *  - Operations which COULD race with the write block: movePrimary, moveChunk, reshardCollection,
 *    createIndex, cross-database renameCollection, compact, autoCompact and convertToCapped. These
 *    clone/move document data or rewrite on-disk data on the shards, so they may fail with
 *    ReplicaSetWritesBlocked-flavored errors.
 *  - Operations which should NOT race with the write block: createCollection, dropCollection,
 *    dropDatabase, shardCollection, dropIndex, same-database rename of an unsharded collection,
 *    same-database rename of a sharded collection, collMod and refineCollectionShardKey. These only
 *    touch config-server metadata and must always succeed regardless of a data shard's write block;
 *    a write-block-flavored error from one of these is a bug and fails the workload loudly.
 *
 * Reads must never be rejected by the write block, so the read state fails loudly if it ever sees
 * ReplicaSetWritesBlocked. Writes and reads are issued with a bounded maxTimeMS: a legitimate write-block
 * rejection fails fast with ReplicaSetWritesBlocked; a MaxTimeMSExpired instead means some lock or
 * critical section was held longer than the write-block window and is flagged as a failure.
 *
 * TTL deletion attempts are also exercised: worker collections carry a TTL index and mixed workers
 * insert already-expired documents, so the TTL monitor attempts deletions while writes are blocked.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_persistence,
 *   featureFlagBlockReplicaSetWrites,
 *  ]
 */

import {ShardingTopologyHelpers} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/sharding_topology_helpers.js";
import {runWriteBlockToggler} from "jstests/concurrency/fsm_workload_helpers/catalog_and_routing/random_ddl_utils.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {disableAllWriteBlocks} from "jstests/libs/block_replica_set_writes_utils.js";

// Same-DB rename critical sections and placement-version refreshes are slow enough on debug
// variants that concurrent CRUD maxTimeMS probes expire waiting on them.
const buildInfo = getBuildInfo();
const skipTest = buildInfo.debug;

let writeBlockToggler;
let writeBlockTogglerStopLatch;

export const $config = (function () {
    const kCollNamePrefix = "ddl_coll_";
    const kInitialCollSize = 100;

    // Every FSM thread is an identical mixed CRUD/DDL worker with its own ddl_coll_<tid> resources.
    const kNumWorkerThreads = 10;

    // Fixed index the createIndex/dropIndex states build and drop, so the two states form a
    // check-then-act pair on the same named index rather than each churning a throwaway index.
    const kUserIndexName = "updateCount_1";

    // Field carrying the TTL index; documents whose ttlDate is older than kTTLExpireAfterSeconds are
    // reaped by the TTL monitor, exercising TTL deletions concurrently with the write block.
    const kTTLFieldName = "ttlDate";
    const kTTLIndexName = "ttlDate_1";
    const kTTLExpireAfterSeconds = 1;

    // How long the write-block thread keeps writes blocked each time. Kept above kOpMaxTimeMS so a
    // write issued during the block reliably hits the block window.
    const kBlockDurationMS = 6000;
    const kUnblockedDurationMS = 1000;

    // Each worker iteration selects exactly one CRUD or DDL state.
    const kNumWorkerIterations = 100;

    // blockReplicaSetWrites only accepts the InsufficientDiskSpace reason. The toggler keeps
    // allowDeletions false as the stricter case, blocking the orphan-deletion cleanup that
    // data-moving DDLs' abort paths perform and maximizing the race surface.
    const kBlockReason = "InsufficientDiskSpace";

    // Client-side timeout for reads and writes. A legitimate write-block/DDL-in-progress rejection
    // is expected well under this; a MaxTimeMSExpired instead signals that some lock is held too
    // long.
    const kOpMaxTimeMS = 5000;

    // Codes expected when a shard's replica-set writes are blocked, or when a data-moving DDL races
    // with another DDL / a write block.
    const kWriteBlockIgnorableCodes = [
        ErrorCodes.ConflictingOperationInProgress,
        ErrorCodes.ReplicaSetWritesBlocked,
        ErrorCodes.MovePrimaryInProgress,
        ErrorCodes.LockBusy,
        ErrorCodes.StaleConfig,
        ErrorCodes.ShardNotFound,
        ErrorCodes.NamespaceNotSharded,
        ErrorCodes.IllegalOperation,
        ErrorCodes.InvalidOptions,
        // A movePrimary joining an already existing one may still have user collections left to
        // clone.
        9046501,
    ];

    // Codes acceptable for catalog-only DDLs due to ordinary concurrent-DDL contention. Deliberately
    // does NOT include any write-block-flavored code: if a catalog-only DDL ever surfaces one of
    // those, that's a bug and the assertion fails loudly.
    const kCatalogOnlyIgnorableCodes = [
        ErrorCodes.ConflictingOperationInProgress,
        ErrorCodes.LockBusy,
        ErrorCodes.NamespaceExists,
        ErrorCodes.NamespaceNotFound,
    ];

    // Writes can fail mid-operation when the replica set hosting the data is blocked, or when a
    // data-moving DDL is in progress on the randomly targeted collection.
    const kCrudIgnorableErrorCodes = [
        ErrorCodes.MovePrimaryInProgress,
        ErrorCodes.ReplicaSetWritesBlocked,
        ErrorCodes.QueryPlanKilled,
        ErrorCodes.StaleConfig,
        ErrorCodes.LockBusy,
        ErrorCodes.ConflictingOperationInProgress,
        ErrorCodes.NamespaceNotFound,
    ];

    // Codes acceptable for reads racing DDL. Deliberately excludes ReplicaSetWritesBlocked: a read
    // must never be rejected by a write block, so seeing that code fails the workload loudly.
    const kReadIgnorableErrorCodes = [
        ErrorCodes.QueryPlanKilled,
        ErrorCodes.StaleConfig,
        ErrorCodes.NamespaceNotFound,
        ErrorCodes.CursorNotFound,
        ErrorCodes.ConflictingOperationInProgress,
    ];

    const data = {
        // A DDL thread's own sharded collection.
        ownCollName: function () {
            return `${kCollNamePrefix}${this.tid}`;
        },
        getOwnColl: function (db) {
            return db.getCollection(this.ownCollName());
        },
        // A random worker-owned collection on which to run CRUD.
        randomWorkerColl: function (db) {
            const t = Random.randInt(kNumWorkerThreads);
            return db.getCollection(`${kCollNamePrefix}${t}`);
        },
        // Two names the scratch collection is toggled between by the same-database rename state.
        scratchCollNames: function () {
            return [
                `${kCollNamePrefix}scratch_${this.tid}_a`,
                `${kCollNamePrefix}scratch_${this.tid}_b`,
            ];
        },
        cappedSrcName: function () {
            return `${kCollNamePrefix}capped_src_${this.tid}`;
        },
        scratchDbName: function (db) {
            return `${db.getName()}_scratchdb_${this.tid}`;
        },
        renameDstDbName: function (db) {
            return `${db.getName()}_rename_dst_${this.tid}`;
        },
        // Dedicated DB for movePrimary only. Must not host reshardCollection or cross-database
        // rename: those hold DB DDL IX long enough that a concurrent movePrimary's pending DB DDL X
        // would starve other IX waiters (create / rename-back) on the same database.
        movePrimaryDbName: function (db) {
            return `${db.getName()}_moveprimary_${this.tid}`;
        },

        // --- State inspection helpers (check-then-act). ---
        collInfo: function (db, name) {
            const infos = db.getCollectionInfos({name: name});
            return infos.length > 0 ? infos[0] : null;
        },
        collExists: function (db, name) {
            return this.collInfo(db, name) !== null;
        },
        isCapped: function (info) {
            return !!(info && info.options && info.options.capped);
        },
        isSharded: function (connCache, ns) {
            const configDB = connCache.rsConns.config.getDB("config");
            return configDB.collections.findOne({_id: ns}) !== null;
        },

        // Returns the list of shard ids visible through mongos.
        getShardIds: function (db) {
            return ShardingTopologyHelpers.getShardNames(db);
        },
        // Runs `func(adminDB)` against the primary of a randomly-chosen shard's replica set.
        withRandomShardAdmin: function (db, func) {
            ShardingTopologyHelpers.executeWithShardInfo(db, this.tid, (shardInfo) => {
                const shardIds = Object.keys(shardInfo.rsConns).filter((id) => id !== "config");
                const shardId = shardIds[Random.randInt(shardIds.length)];
                func(shardInfo.rsConns[shardId].getDB("admin"));
            });
        },
        // Executes a best-effort CRUD write via runCommand with a bounded maxTimeMS, swallowing
        // only the errors expected when writes are blocked or a DDL is in progress on the
        // (unowned) target. A MaxTimeMSExpired is treated as a failure: a legitimate rejection
        // should be fast. Collection helpers are avoided so maxTimeMS is applied as a true
        // command-level option.
        tolerantWrite: function (db, cmd) {
            const res = db.runCommand(Object.assign({}, cmd, {maxTimeMS: kOpMaxTimeMS}));
            if (res.ok) {
                if (res.writeErrors) {
                    for (const writeErr of res.writeErrors) {
                        assert.contains(writeErr.code, kCrudIgnorableErrorCodes, () => tojson(res));
                    }
                }
                return;
            }
            assert.neq(
                res.code,
                ErrorCodes.MaxTimeMSExpired,
                () =>
                    `Write timed out after ${kOpMaxTimeMS}ms instead of failing fast; ` +
                    `some lock/critical section may be held longer than intended: ${tojson(res)}`,
            );
            assert.contains(res.code, kCrudIgnorableErrorCodes, () => tojson(res));
        },
    };

    const states = {
        init: function (db, collName, connCache) {
            // Every mixed worker owns resources. Initialize them up front so CRUD and every DDL state
            // can assume the main collection exists and is sharded.
            const coll = this.getOwnColl(db);
            jsTest.log.info("Initializing worker collection", {coll: coll.getFullName()});

            assert.soon(() => {
                const res = db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}});
                if (res.ok) {
                    return true;
                }
                assert.contains(res.code, kWriteBlockIgnorableCodes, () => tojson(res));
                return false;
            }, "Failed to shard the initial collection");

            // TTL index so the TTL monitor reaps expired docs (inserted by mixed workers) throughout
            // the run, racing the write block.
            assert.soon(() => {
                const res = db.runCommand({
                    createIndexes: coll.getName(),
                    indexes: [
                        {
                            key: {[kTTLFieldName]: 1},
                            name: kTTLIndexName,
                            expireAfterSeconds: kTTLExpireAfterSeconds,
                        },
                    ],
                });
                if (res.ok) {
                    return true;
                }
                assert.contains(
                    res.code,
                    [...kWriteBlockIgnorableCodes, ErrorCodes.IndexBuildAborted],
                    () => tojson(res),
                );
                // An index build writes index data on the shards, so a concurrent write block can
                // abort it. Tolerate that here, but only if the abort was actually caused by write
                // blocking.
                if (res.code === ErrorCodes.IndexBuildAborted) {
                    assert(
                        /Write blocking/.test(res.errmsg),
                        "IndexBuildAborted for a reason other than write blocking",
                        {res},
                    );
                }
                return false;
            }, "Failed to create the TTL index");

            this.tolerantWrite(db, {
                insert: coll.getName(),
                documents: Array.from({length: kInitialCollSize}, () => ({
                    _id: new ObjectId(),
                    updateCount: 0,
                })),
            });
        },

        insert: function (db, collName, connCache) {
            const coll = this.randomWorkerColl(db);
            this.tolerantWrite(db, {
                insert: coll.getName(),
                documents: [{_id: new ObjectId(), updateCount: 0}],
            });
        },
        update: function (db, collName, connCache) {
            const coll = this.randomWorkerColl(db);
            this.tolerantWrite(db, {
                update: coll.getName(),
                updates: [{q: {}, u: {$inc: {updateCount: 1}}}],
            });
        },
        delete: function (db, collName, connCache) {
            const coll = this.randomWorkerColl(db);
            this.tolerantWrite(db, {
                delete: coll.getName(),
                deletes: [{q: {}, limit: 1}],
            });
        },
        insertExpired: function (db, collName, connCache) {
            // ttlDate well in the past so the TTL monitor deletes it shortly after insertion,
            // actively driving TTL deletions that race with the write block.
            const coll = this.randomWorkerColl(db);
            this.tolerantWrite(db, {
                insert: coll.getName(),
                documents: [
                    {
                        _id: new ObjectId(),
                        updateCount: 0,
                        [kTTLFieldName]: new Date(Date.now() - 3600 * 1000),
                    },
                ],
            });
        },
        read: function (db, collName, connCache) {
            const coll = this.randomWorkerColl(db);
            // Reads must never be rejected by the write block. Tolerate only routing/DDL-race
            // codes; a ReplicaSetWritesBlocked here (not in the tolerated list) fails loudly. A
            // MaxTimeMSExpired past kOpMaxTimeMS still fails loudly as a genuine lock/critical-
            // section hang.
            try {
                coll.find().maxTimeMS(kOpMaxTimeMS).itcount();
            } catch (err) {
                assert.contains(err.code, kReadIgnorableErrorCodes, () => tojson(err));
            }
        },

        // DDLs that could race with the block
        movePrimary: function (db, collName, connCache) {
            // Target the dedicated movePrimary DB, never the main FSM DB (reshard / cross-DB rename)
            // or rename_dst / scratch DBs.
            const targetDB = db.getSiblingDB(this.movePrimaryDbName(db));
            const seedCollName = `${kCollNamePrefix}moveprimary_seed_${this.tid}`;
            if (!this.collExists(targetDB, seedCollName)) {
                assert.commandWorkedOrFailedWithCode(
                    targetDB.runCommand({create: seedCollName}),
                    kCatalogOnlyIgnorableCodes,
                );
            }
            const shardIds = this.getShardIds(db);
            const toShard = shardIds[Random.randInt(shardIds.length)];
            jsTest.log.info("Running movePrimary", {db: targetDB.getName(), to: toShard});
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({movePrimary: targetDB.getName(), to: toShard}),
                kWriteBlockIgnorableCodes,
            );
        },
        moveChunk: function (db, collName, connCache) {
            const coll = this.getOwnColl(db);
            const configDB = connCache.rsConns.config.getDB("config");
            const chunkDoc = findChunksUtil.findOneChunkByNs(configDB, coll.getFullName());
            if (!chunkDoc) {
                return;
            }
            const destinationShards = this.getShardIds(db).filter((s) => s !== chunkDoc.shard);
            if (destinationShards.length === 0) {
                return;
            }
            const toShard = destinationShards[Random.randInt(destinationShards.length)];
            jsTest.log.info("Running moveChunk", {coll: coll.getFullName(), to: toShard});
            // The write block rejects new incoming migrations, so a moveChunk started while the recipient
            // is blocked may fail with ReplicaSetWritesBlocked. Migrations that were already in flight
            // when the block was enabled are allowed to finish. A moveChunk racing a concurrent movePrimary
            // on the same namespace legitimately aborts the migration; mongos surfaces this as a top-level
            // OperationFailed wrapping MovePrimaryInProgress, so that specific OperationFailed is tolerated.
            // The same wrapping happens when the recipient aborts because committing would drop PIT-reachable
            // ownership history after a concurrent reshard/refine (ConflictingOperationInProgress).
            // history after a concurrent reshard/refine (ConflictingOperationInProgress).
            const res = db.adminCommand({
                moveChunk: coll.getFullName(),
                bounds: [chunkDoc.min, chunkDoc.max],
                to: toShard,
            });
            if (
                res.code === ErrorCodes.OperationFailed &&
                (/MovePrimaryInProgress/.test(res.errmsg) ||
                    /ConflictingOperationInProgress/.test(res.errmsg))
            ) {
                return;
            }
            assert.commandWorkedOrFailedWithCode(res, kWriteBlockIgnorableCodes);
        },
        reshardCollection: function (db, collName, connCache) {
            const coll = this.getOwnColl(db);
            const newShardKeyField = `reshardKey_${this.tid}_${Random.randInt(1 << 30)}`;
            jsTest.log.info("Running reshardCollection", {
                coll: coll.getFullName(),
                key: newShardKeyField,
            });
            // Resharding is a cluster-wide singleton, so a concurrent reshard on another thread's
            // collection legitimately rejects this one with ReshardCollectionInProgress.
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({
                    reshardCollection: coll.getFullName(),
                    key: {[newShardKeyField]: 1},
                    numInitialChunks: 1,
                }),
                [...kWriteBlockIgnorableCodes, ErrorCodes.ReshardCollectionInProgress],
            );
        },
        createIndex: function (db, collName, connCache) {
            const coll = this.getOwnColl(db);
            if (coll.getIndexes().some((ix) => ix.name === kUserIndexName)) {
                return;
            }
            jsTest.log.info("Creating index", {coll: coll.getFullName(), index: kUserIndexName});
            // An index build writes index data on the shards, so it can legitimately race with a
            // write block.
            const res = db.runCommand({
                createIndexes: coll.getName(),
                indexes: [{key: {updateCount: 1}, name: kUserIndexName}],
            });
            assert.commandWorkedOrFailedWithCode(res, [
                ...kWriteBlockIgnorableCodes,
                ErrorCodes.IndexBuildAborted,
            ]);
            // Check that IndexBuildAborted returned because of replica set write block enabled
            if (res.code === ErrorCodes.IndexBuildAborted) {
                assert(
                    /Write blocking/.test(res.errmsg),
                    "IndexBuildAborted for a reason other than write blocking",
                    {res},
                );
            }
        },
        compact: function (db, collName, connCache) {
            const collName_ = this.ownCollName();
            jsTest.log.info("Running compact", {coll: `${db.getName()}.${collName_}`});
            // compact is not allowed through mongos; it must run directly against a shard node. It
            // rewrites on-disk data, so it can race with a write block. The collection may not exist
            // on the chosen shard, so NamespaceNotFound is tolerated.
            this.withRandomShardAdmin(db, (adminDB) => {
                const shardDB = adminDB.getSiblingDB(db.getName());
                assert.commandWorkedOrFailedWithCode(
                    shardDB.runCommand({compact: collName_, force: true}),
                    [...kWriteBlockIgnorableCodes, ErrorCodes.NamespaceNotFound],
                );
            });
        },
        autoCompact: function (db, collName, connCache) {
            jsTest.log.info("Toggling autoCompact", {db: db.getName()});
            // autoCompact is a per-node storage-engine setting; run it against a random shard's
            // primary. Like compact it rewrites on-disk data and can race with a write block.
            this.withRandomShardAdmin(db, (adminDB) => {
                const enableRes = adminDB.runCommand({autoCompact: true, freeSpaceTargetMB: 1});
                if (
                    enableRes.code === ErrorCodes.CommandNotSupported ||
                    enableRes.code === ErrorCodes.CommandNotFound
                ) {
                    jsTest.log.info("autoCompact not supported, skipping", {res: enableRes});
                    return;
                }
                assert.commandWorkedOrFailedWithCode(enableRes, [
                    ...kWriteBlockIgnorableCodes,
                    ErrorCodes.AlreadyInitialized,
                    ErrorCodes.ObjectIsBusy,
                ]);
                assert.commandWorkedOrFailedWithCode(adminDB.runCommand({autoCompact: false}), [
                    ...kWriteBlockIgnorableCodes,
                    ErrorCodes.ObjectIsBusy,
                ]);
            });
        },
        renameCollectionAcrossDBs: function (db, collName, connCache) {
            // Rename the (unsharded) capped source across databases; requires it to exist and be
            // unsharded. It is recreated by the capped-source state, so skip when absent.
            const srcName = this.cappedSrcName();
            const srcInfo = this.collInfo(db, srcName);
            if (!srcInfo || this.isSharded(connCache, `${db.getName()}.${srcName}`)) {
                return;
            }
            const toDB = db.getSiblingDB(this.renameDstDbName(db));
            const toCollName = `${kCollNamePrefix}renamed_${this.tid}`;
            jsTest.log.info("Renaming collection across databases", {
                from: `${db.getName()}.${srcName}`,
                to: `${toDB.getName()}.${toCollName}`,
            });
            // Cross-database rename copies documents to the destination database's primary shard, so
            // it moves data and can race with the write block.
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({
                    renameCollection: `${db.getName()}.${srcName}`,
                    to: `${toDB.getName()}.${toCollName}`,
                    dropTarget: true,
                }),
                // Cross-DB rename requires the source and the destination DB's primary shard to be
                // co-located; if they ever diverge (e.g. independent primary placement), mongos fails
                // with CommandFailed ("Source and destination collections must be on same shard").
                [...kWriteBlockIgnorableCodes, ErrorCodes.CommandFailed],
            );
        },
        convertToCapped: function (db, collName, connCache) {
            const srcName = this.cappedSrcName();
            const srcInfo = this.collInfo(db, srcName);
            if (
                !srcInfo ||
                this.isCapped(srcInfo) ||
                this.isSharded(connCache, `${db.getName()}.${srcName}`)
            ) {
                return;
            }
            jsTest.log.info("Converting collection to capped", {
                coll: `${db.getName()}.${srcName}`,
            });
            // convertToCapped rebuilds on-disk data as a capped collection, so it can race with the
            // write block.
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({convertToCapped: srcName, size: 8192}),
                [...kWriteBlockIgnorableCodes, ErrorCodes.NamespaceNotFound],
            );
        },

        // DDLs that should NOT race with the block
        createScratchCollection: function (db, collName, connCache) {
            const [nameA, nameB] = this.scratchCollNames();
            if (this.collExists(db, nameA) || this.collExists(db, nameB)) {
                return;
            }
            jsTest.log.info("Creating scratch collection", {coll: `${db.getName()}.${nameA}`});
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({create: nameA}),
                kCatalogOnlyIgnorableCodes,
            );
        },
        shardScratchCollection: function (db, collName, connCache) {
            const [nameA, nameB] = this.scratchCollNames();
            const name = this.collExists(db, nameA)
                ? nameA
                : this.collExists(db, nameB)
                  ? nameB
                  : null;
            if (name === null || this.isSharded(connCache, `${db.getName()}.${name}`)) {
                return;
            }
            jsTest.log.info("Sharding scratch collection", {coll: `${db.getName()}.${name}`});
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({shardCollection: `${db.getName()}.${name}`, key: {_id: 1}}),
                kCatalogOnlyIgnorableCodes,
            );
        },
        renameScratchCollection: function (db, collName, connCache) {
            // Rename the scratch collection to its other name (same database). Depending on whether
            // shardScratchCollection has run, this exercises both the unsharded and the sharded
            // same-database rename code paths over time.
            const [nameA, nameB] = this.scratchCollNames();
            const aExists = this.collExists(db, nameA);
            const bExists = this.collExists(db, nameB);
            let from, to;
            if (aExists && !bExists) {
                [from, to] = [nameA, nameB];
            } else if (bExists && !aExists) {
                [from, to] = [nameB, nameA];
            } else {
                return;
            }
            const sharded = this.isSharded(connCache, `${db.getName()}.${from}`);
            jsTest.log.info("Renaming scratch collection within the same database", {
                from: `${db.getName()}.${from}`,
                to: `${db.getName()}.${to}`,
                sharded: sharded,
            });
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({
                    renameCollection: `${db.getName()}.${from}`,
                    to: `${db.getName()}.${to}`,
                }),
                kCatalogOnlyIgnorableCodes,
            );
        },
        dropScratchCollection: function (db, collName, connCache) {
            const [nameA, nameB] = this.scratchCollNames();
            const name = this.collExists(db, nameA)
                ? nameA
                : this.collExists(db, nameB)
                  ? nameB
                  : null;
            if (name === null) {
                return;
            }
            jsTest.log.info("Dropping scratch collection", {coll: `${db.getName()}.${name}`});
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({drop: name}),
                kCatalogOnlyIgnorableCodes,
            );
        },
        createCappedSource: function (db, collName, connCache) {
            // (Re)create the (unsharded) capped-experiment source when absent so convertToCapped /
            // renameCollectionAcrossDBs have something to operate on.
            const srcName = this.cappedSrcName();
            if (this.collExists(db, srcName)) {
                return;
            }
            jsTest.log.info("Creating capped-experiment source", {
                coll: `${db.getName()}.${srcName}`,
            });
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({create: srcName}),
                kCatalogOnlyIgnorableCodes,
            );
        },
        dropCappedCollection: function (db, collName, connCache) {
            // Drop the source so the capped experiment can start over.
            const srcName = this.cappedSrcName();
            if (!this.collExists(db, srcName)) {
                return;
            }
            const name = srcName;
            jsTest.log.info("Dropping capped-experiment collection", {
                coll: `${db.getName()}.${name}`,
            });
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({drop: name}),
                kCatalogOnlyIgnorableCodes,
            );
        },
        dropIndex: function (db, collName, connCache) {
            const coll = this.getOwnColl(db);
            if (!coll.getIndexes().some((ix) => ix.name === kUserIndexName)) {
                return;
            }
            jsTest.log.info("Dropping index", {coll: coll.getFullName(), index: kUserIndexName});
            // Dropping an index removes only catalog metadata and index files; it performs no
            // replicated user writes, so it never races with the write block. IndexNotFound is
            // tolerated in case a concurrent op removed the index first.
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({dropIndexes: coll.getName(), index: kUserIndexName}),
                [...kCatalogOnlyIgnorableCodes, ErrorCodes.IndexNotFound],
            );
        },
        renameMainCollectionSameDB: function (db, collName, connCache) {
            const coll = this.getOwnColl(db);
            const tempNs = `${db.getName()}.${kCollNamePrefix}main_tmp_${this.tid}`;
            jsTest.log.info("Renaming own sharded collection within the same database", {
                from: coll.getFullName(),
                to: tempNs,
            });
            // Renaming a sharded collection within a database is a config-server metadata operation
            // (no document data moves), so it must succeed regardless of any shard's write block.
            // Rename to a temp name and back so CRUD and other states keep finding it; the
            // rename-back is the single logical undo of the same operation, not a different DDL.
            const renamedAway = assert.commandWorkedOrFailedWithCode(
                db.adminCommand({
                    renameCollection: coll.getFullName(),
                    to: tempNs,
                    dropTarget: true,
                }),
                kCatalogOnlyIgnorableCodes,
            );
            if (!renamedAway.ok) {
                return;
            }
            assert.soon(() => {
                const res = db.adminCommand({
                    renameCollection: tempNs,
                    to: coll.getFullName(),
                    dropTarget: true,
                });
                if (res.ok) {
                    return true;
                }
                assert.contains(res.code, kCatalogOnlyIgnorableCodes, () => tojson(res));
                return false;
            }, "Failed to rename the collection back to its original name");
        },
        collMod: function (db, collName, connCache) {
            const coll = this.getOwnColl(db);
            jsTest.log.info("Running collMod", {coll: coll.getFullName()});
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({collMod: coll.getName(), validationLevel: "off"}),
                kCatalogOnlyIgnorableCodes,
            );
        },
        refineCollectionShardKey: function (db, collName, connCache) {
            const coll = this.getOwnColl(db);
            const configDB = connCache.rsConns.config.getDB("config");
            const collDoc = configDB.collections.findOne({_id: coll.getFullName()});
            if (!collDoc) {
                return;
            }
            // Fresh field every time so the proposed key is always a genuine extension of the
            // current shard key.
            const suffixField = `refineSuffix_${this.tid}_${Random.randInt(1 << 30)}`;
            const newKey = Object.assign({}, collDoc.key, {[suffixField]: 1});
            jsTest.log.info("Running refineCollectionShardKey", {
                coll: coll.getFullName(),
                key: newKey,
            });
            // refineCollectionShardKey requires an index prefixed by the proposed key. Building that
            // index writes index data on the shards, so it can race with the write block; skip the
            // refine if it doesn't get created.
            const indexRes = db.runCommand({
                createIndexes: coll.getName(),
                indexes: [{key: newKey, name: `${suffixField}_refine`}],
            });
            assert.commandWorkedOrFailedWithCode(indexRes, [
                ...kWriteBlockIgnorableCodes,
                ErrorCodes.IndexBuildAborted,
            ]);
            // An index build writes index data on the shards, so a concurrent write block can abort
            // it. Tolerate that here, but only if the abort was actually caused by write blocking.
            if (indexRes.code === ErrorCodes.IndexBuildAborted) {
                assert(
                    /Write blocking/.test(indexRes.errmsg),
                    "IndexBuildAborted for a reason other than write blocking",
                    {indexRes},
                );
            }
            if (!indexRes.ok) {
                return;
            }
            // The refine itself only updates config-server metadata (no data movement), so it must
            // not surface a write-block code. StaleConfig is tolerated because a concurrent
            // reshardCollection may change the shard key underneath us.
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({refineCollectionShardKey: coll.getFullName(), key: newKey}),
                [...kCatalogOnlyIgnorableCodes, ErrorCodes.StaleConfig],
            );
        },
        createScratchDbCollection: function (db, collName, connCache) {
            // Materialize the per-thread scratch database by creating an empty collection in it, so
            // the dropScratchDatabase state has something to remove.
            const scratchDB = db.getSiblingDB(this.scratchDbName(db));
            const scratchCollName = `${kCollNamePrefix}scratchdb_coll_${this.tid}`;
            if (this.collExists(scratchDB, scratchCollName)) {
                return;
            }
            jsTest.log.info("Creating collection in scratch database", {
                coll: `${scratchDB.getName()}.${scratchCollName}`,
            });
            assert.commandWorkedOrFailedWithCode(
                scratchDB.runCommand({create: scratchCollName}),
                kCatalogOnlyIgnorableCodes,
            );
        },
        dropScratchDatabase: function (db, collName, connCache) {
            // Drop the per-thread scratch database. With its collection empty this moves no document
            // data, so it is catalog-only and must succeed regardless of any shard's write block.
            const scratchDB = db.getSiblingDB(this.scratchDbName(db));
            jsTest.log.info("Dropping scratch database", {db: scratchDB.getName()});
            assert.commandWorkedOrFailedWithCode(
                scratchDB.runCommand({dropDatabase: 1}),
                kCatalogOnlyIgnorableCodes,
            );
        },
    };

    const setup = function (db, collName, cluster) {
        if (skipTest) {
            return;
        }
        // Make the TTL monitor run frequently so TTL deletions actively race with the write block
        // rather than firing at most once per default 60s interval.
        cluster.executeOnMongodNodes((nodeDB) => {
            assert.commandWorked(nodeDB.adminCommand({setParameter: 1, ttlMonitorSleepSecs: 1}));
        });

        const shards = assert
            .commandWorked(db.adminCommand({listShards: 1}))
            .shards.filter((shard) => shard._id !== "config")
            .map((shard) => ({name: shard._id, host: shard.host}));
        assert.gt(shards.length, 0, "Expected at least one data shard");

        writeBlockTogglerStopLatch = new CountDownLatch(1);
        writeBlockToggler = new Thread(
            runWriteBlockToggler,
            shards,
            writeBlockTogglerStopLatch,
            kBlockReason,
            kBlockDurationMS,
            kUnblockedDurationMS,
            Random.randInt(1e13),
            false /* allowDeletions */,
        );
        writeBlockToggler.start();
    };

    const teardown = function (db, collName, cluster) {
        if (skipTest) {
            return;
        }
        // Stop the write block toggler
        writeBlockTogglerStopLatch.countDown();
        writeBlockToggler.join();

        // Disable the replica set write block on every shard so no shard is left blocked for the
        // suite's post-workload hooks (e.g. CleanupConcurrencyWorkloads, which drops the databases).
        disableAllWriteBlocks(ShardingTopologyHelpers.getShardInfo(db, 0), kBlockReason);

        const inconsistencies = db.checkMetadataConsistency().toArray();
        assert.eq(0, inconsistencies.length, () => tojson(inconsistencies));
    };

    // Every mixed worker uses the same distribution: 70% CRUD and 30% DDL.
    const standardTransition = {
        insert: 0.14,
        update: 0.14,
        delete: 0.14,
        insertExpired: 0.14,
        read: 0.14,
        // Could race with the write block.
        movePrimary: 0.024,
        moveChunk: 0.018,
        reshardCollection: 0.024,
        createIndex: 0.024,
        compact: 0.018,
        autoCompact: 0.018,
        renameCollectionAcrossDBs: 0.012,
        convertToCapped: 0.018,
        // cloneCollectionAsCapped is deliberately not exercised here: mongos does not support it, so
        // it can only be issued directly against a shard node. Running it that way bypasses
        // serialization with movePrimary, which can strand the cloned collection on a non-primary
        // shard and leave the catalog inconsistent.
        // Should not race with the write block.
        createScratchCollection: 0.012,
        shardScratchCollection: 0.012,
        renameScratchCollection: 0.012,
        dropScratchCollection: 0.012,
        createCappedSource: 0.012,
        dropCappedCollection: 0.012,
        dropIndex: 0.012,
        renameMainCollectionSameDB: 0.012,
        collMod: 0.012,
        refineCollectionShardKey: 0.012,
        createScratchDbCollection: 0.012,
        dropScratchDatabase: 0.012,
    };

    const transitions = {
        init: standardTransition,
        insert: standardTransition,
        update: standardTransition,
        delete: standardTransition,
        insertExpired: standardTransition,
        read: standardTransition,
        movePrimary: standardTransition,
        moveChunk: standardTransition,
        reshardCollection: standardTransition,
        createIndex: standardTransition,
        compact: standardTransition,
        autoCompact: standardTransition,
        renameCollectionAcrossDBs: standardTransition,
        convertToCapped: standardTransition,
        createScratchCollection: standardTransition,
        shardScratchCollection: standardTransition,
        renameScratchCollection: standardTransition,
        dropScratchCollection: standardTransition,
        createCappedSource: standardTransition,
        dropCappedCollection: standardTransition,
        dropIndex: standardTransition,
        renameMainCollectionSameDB: standardTransition,
        collMod: standardTransition,
        refineCollectionShardKey: standardTransition,
        createScratchDbCollection: standardTransition,
        dropScratchDatabase: standardTransition,
    };

    return {
        // All FSM threads are identical mixed CRUD/DDL workers.
        threadCount: kNumWorkerThreads,
        iterations: skipTest ? 0 : kNumWorkerIterations,
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true,
    };
})();

/**
 * Stress test to exercise AsyncResultsMerger's delayed retry during a concurrent getMore.
 *
 * Multiple threads concurrently issue getMores on shared cursors against a sharded collection.
 * Rate-limited remote getMores (configured by the suite via failIngressRequestRateLimiting
 * failpoint) cause the ARM to schedule delayed retries via the executor. Those retries can fire
 * asynchronously which can generate interesting orderings with other getMores on the same cursor.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_getmore,
 *   assumes_balancer_off,
 *   incompatible_with_concurrency_simultaneous,
 *   catches_command_failures,
 *   can_leak_idle_cursors,
 *   # TODO BACKPORT-29703: remove when the IFRContext egress-metadata caching backport lands on v9.0.
 *   backport_required_multiversion,
 * ]
 */
export const $config = (function () {
    const kBatchSize = 1;
    const kNumSharedCursors = 6;
    const kNumDocsShardA = 200;
    const kNumDocsShardB = 5;
    // How many getMores to run during one iteration.
    const kGetMoreBurst = 4;

    let data = {
        shardKey: {_id: 1},
        cursorPool: [],
        cursorIdx: 0,
        getMoreBurst: kGetMoreBurst,

        /**
         * Opens a new cursor with a fresh lsid and, if successful, pushes it onto the cursor
         * pool. Returns true if a cursor was opened and added, false otherwise.
         */
        openCursor: function openCursor(db, collName) {
            const lsid = {id: UUID()};
            const res = db.runCommand({
                find: collName,
                filter: {},
                batchSize: kBatchSize,
                lsid: lsid,
            });
            if (
                res.ok &&
                res.cursor &&
                bsonWoCompare({_: res.cursor.id}, {_: NumberLong(0)}) !== 0
            ) {
                return {
                    cursorId: tojson(res.cursor.id),
                    lsid: tojson(lsid),
                };
            }
            return null;
        },
    };

    let states = {
        init: function init(db, collName) {},

        hammerGetMore: function hammerGetMore(db, collName) {
            if (this.cursorPool.length === 0) return;

            const entry = this.cursorPool[this.cursorIdx % this.cursorPool.length];
            this.cursorIdx += 1;

            // The stored values are tojson() strings (e.g. '{ "id" : UUID("...") }'). Wrap in
            // parentheses so eval() parses them as expressions — a leading '{' would otherwise be
            // parsed as a block statement and fail with "SyntaxError: unexpected token: ':'".
            const cursorId = eval("(" + entry.cursorId + ")");
            const lsid = eval("(" + entry.lsid + ")");

            for (let i = 0; i < this.getMoreBurst; i++) {
                const res = db.runCommand({
                    getMore: cursorId,
                    collection: collName,
                    batchSize: kBatchSize,
                    lsid: lsid,
                });

                if (res.code === ErrorCodes.CursorInUse) {
                    continue;
                } else if (res.code === ErrorCodes.CursorNotFound) {
                    // Remove this from the pool so we don't continue to send getMores with this
                    // cursor ID.
                    this.cursorPool = this.cursorPool.filter((c) => c.cursorId !== entry.cursorId);
                    break;
                } else if (!res.ok) {
                    jsTest.log.info("getMore failed", {code: res.code, errmsg: res.errmsg});
                    break;
                }
                if (bsonWoCompare({_: res.cursor.id}, {_: NumberLong(0)}) === 0) {
                    this.cursorPool = this.cursorPool.filter((c) => c.cursorId !== entry.cursorId);
                    break;
                }
            }
        },

        /**
         * Open a new set of cursors for continued iteration. Ensures we don't run out of cursors to
         * hammer on if the cursors are exhausted.
         */
        refreshCursors: function refreshCursors(db, collName) {
            const needed = kNumSharedCursors - this.cursorPool.length;
            if (needed <= 0) return;

            for (let i = 0; i < needed; i++) {
                const newCursor = this.openCursor(db, collName);
                if (newCursor !== null) {
                    this.cursorPool.push(newCursor);
                }
            }
        },
    };

    let transitions = {
        init: {hammerGetMore: 1},
        hammerGetMore: {hammerGetMore: 0.85, refreshCursors: 0.15},
        refreshCursors: {hammerGetMore: 1},
    };

    function setup(db, collName, cluster) {
        const shardNames = Object.keys(cluster.getSerializedCluster().shards);
        assert.gte(shardNames.length, 2, "Need at least 2 shards");

        const adminDb = db.getSiblingDB("admin");

        assert.commandWorked(
            adminDb.runCommand({
                enableSharding: db.getName(),
            }),
        );

        const dbPrimary = db.getSiblingDB("config").databases.findOne({_id: db.getName()}).primary;
        const otherShard = shardNames.find((s) => s !== dbPrimary);

        assert.commandWorkedOrFailedWithCode(
            adminDb.runCommand({
                shardCollection: `${db.getName()}.${collName}`,
                key: {_id: 1},
            }),
            [ErrorCodes.AlreadyInitialized],
        );

        assert.commandWorked(
            adminDb.runCommand({
                split: `${db.getName()}.${collName}`,
                middle: {_id: kNumDocsShardA},
            }),
        );

        assert.commandWorked(
            adminDb.runCommand({
                moveChunk: `${db.getName()}.${collName}`,
                find: {_id: kNumDocsShardA},
                to: otherShard,
            }),
        );

        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < kNumDocsShardA; i++) {
            bulk.insert({_id: i});
        }
        for (let i = kNumDocsShardA; i < kNumDocsShardA + kNumDocsShardB; i++) {
            bulk.insert({_id: i});
        }
        assert.commandWorked(bulk.execute());

        // Use failCommand scoped to getMore on this namespace rather than the suite's broad
        // failIngressRequestRateLimiting. The global failpoint randomly rejects every non-exempt
        // authenticated request (heartbeats, replication, internal sharding commands, etc.) which
        // can starve infrastructure and blow past the 600 s awaitReplication timeout. failCommand
        // is configured directly on mongod nodes — not via the suite-level setParameter — so the
        // activation probability can be raised above the suite default without collateral damage.
        cluster.executeOnMongodNodes((mongodAdmin) => {
            mongodAdmin.adminCommand({
                configureFailPoint: "failCommand",
                mode: {activationProbability: 0.5},
                data: {
                    errorCode: ErrorCodes.IngressRequestRateLimitExceeded,
                    failCommands: ["getMore"],
                    failInternalCommands: true,
                    namespace: `${db.getName()}.${collName}`,
                    errorLabels: ["SystemOverloadedError", "RetryableError"],
                },
            });
        });

        this.cursorPool = [];
        for (let i = 0; i < kNumSharedCursors; i++) {
            const newCursor = this.openCursor(db, collName);
            assert.neq(null, newCursor, "expected an open cursor");
            this.cursorPool.push(newCursor);
        }
    }

    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes((mongodAdmin) => {
            mongodAdmin.adminCommand({
                configureFailPoint: "failCommand",
                mode: "off",
            });
        });

        for (const entry of this.cursorPool) {
            try {
                db.runCommand({
                    killCursors: collName,
                    cursors: [eval("(" + entry.cursorId + ")")],
                });
            } catch (e) {
                jsTest.log.info("killCursors failed during teardown", {error: e.toString()});
            }
        }
    }

    return {
        threadCount: 24,
        iterations: 300,
        startState: "init",
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
    };
})();

/**
 * When a client sends {readConcern: {afterClusterTime: T}} without a level, the server must
 * apply the cluster-wide default read concern level instead of silently falling back to "local".
 * Same scenario is verified for a replica set (mongod path) and a sharded cluster (mongos path).
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    restartReplicationOnSecondaries,
    stopReplicationOnSecondaries,
} from "jstests/libs/write_concern_util.js";

const dbName = "cwrwc_aftercluster_db";
const collName = "cwrwc_aftercluster_coll";
const kBlockMs = 1500;

let _idCounter = 100;
const nextId = () => ++_idCounter;

// Commands whose CommandInvocation::supportsReadConcern returns defaultReadConcernPermit=OK,
// i.e. _extractReadConcern applies the CWRC default for these. Other commands
// (mapReduce / findAndModify / listIndexes / listCollections / listDatabases / collStats /
// dbStats / etc.) explicitly opt out via defaultReadConcernNotPermitted, so the dispatcher
// falls back to the implicit default (level=local) and the CWRC default is not applied —
// by design, not a SERVER-126299 regression.
function makeBlockingCommands(coll) {
    return [
        {name: "find", cmd: {find: coll, filter: {}}},
        {name: "aggregate", cmd: {aggregate: coll, pipeline: [], cursor: {}}},
        {name: "count", cmd: {count: coll}},
        {name: "distinct", cmd: {distinct: coll, key: "a"}},
    ];
}

function runTests({label, makeFixture, teardown, getRouter, getShardRs, getMetricsConn}) {
    describe(label, function () {
        let fixture;
        let router;
        let shardRs;
        let metricsConn;
        let testDB;
        let testColl;

        before(() => {
            fixture = makeFixture();
            router = getRouter(fixture);
            shardRs = getShardRs(fixture);
            metricsConn = getMetricsConn(fixture);
            testDB = router.getDB(dbName);
            testColl = testDB.getCollection(collName);

            assert.commandWorked(
                router.adminCommand({
                    setDefaultRWConcern: 1,
                    defaultReadConcern: {level: "majority"},
                    writeConcern: {w: "majority"},
                }),
            );

            assert.commandWorked(testColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));
        });

        after(() => {
            restartReplicationOnSecondaries(shardRs);
            teardown(fixture);
        });

        for (const {name, cmd} of makeBlockingCommands(collName)) {
            it(`blocks ${name} with explicit afterClusterTime and no level when CWRWC level is majority`, () => {
                stopReplicationOnSecondaries(shardRs, false);
                try {
                    // w:1 insert so replication is stopped and the majority commit point
                    // does not advance to this op.
                    const insertRes = assert.commandWorked(
                        testDB.runCommand({
                            insert: collName,
                            documents: [{_id: nextId()}],
                            writeConcern: {w: 1},
                        }),
                    );
                    const ts = insertRes.operationTime;

                    // Before the fix this would return immediately at level=local.
                    assert.commandFailedWithCode(
                        testDB.runCommand(
                            Object.assign({}, cmd, {
                                readConcern: {afterClusterTime: ts},
                                maxTimeMS: kBlockMs,
                            }),
                        ),
                        ErrorCodes.MaxTimeMSExpired,
                        `expected ${name} to block waiting for majority commit point to reach afterClusterTime`,
                    );
                } finally {
                    restartReplicationOnSecondaries(shardRs);
                    shardRs.awaitReplication();
                }
            });
        }

        it("reflects the merged majority level in serverStatus readConcern metrics", () => {
            const getCounters = () => {
                const status = assert.commandWorked(
                    metricsConn.adminCommand({serverStatus: 1, defaultRWConcern: false}),
                );
                return status.readConcernCounters.nonTransactionOps;
            };

            const insertRes = assert.commandWorked(
                testDB.runCommand({
                    insert: collName,
                    documents: [{_id: 2}],
                    writeConcern: {w: "majority"},
                }),
            );
            const ts = insertRes.operationTime;

            const beforeCounters = getCounters();
            assert.commandWorked(
                testDB.runCommand({find: collName, readConcern: {afterClusterTime: ts}}),
            );
            const afterCounters = getCounters();

            const beforeMaj =
                (beforeCounters.noneInfo &&
                    beforeCounters.noneInfo.CWRC &&
                    beforeCounters.noneInfo.CWRC.majority) ||
                0;
            const afterMaj =
                (afterCounters.noneInfo &&
                    afterCounters.noneInfo.CWRC &&
                    afterCounters.noneInfo.CWRC.majority) ||
                0;
            assert.eq(
                1,
                afterMaj - beforeMaj,
                "expected noneInfo.CWRC.majority to increment by 1",
                {
                    beforeCounters,
                    afterCounters,
                },
            );
        });

        it("blocks a causal-consistency session read when CWRWC level is majority", () => {
            stopReplicationOnSecondaries(shardRs, false);
            let session;
            try {
                session = router.startSession({causalConsistency: true});
                const sessionDB = session.getDatabase(dbName);

                // Session tracks the write's operationTime; subsequent reads ship
                // {readConcern: {afterClusterTime: <op>}} with no level (per SPEC-970).
                assert.commandWorked(
                    sessionDB.runCommand({
                        insert: collName,
                        documents: [{_id: 3}],
                        writeConcern: {w: 1},
                    }),
                );

                assert.commandFailedWithCode(
                    sessionDB.runCommand({find: collName, maxTimeMS: kBlockMs}),
                    ErrorCodes.MaxTimeMSExpired,
                    "expected causal-consistency session read to block waiting for majority",
                );
            } finally {
                if (session !== undefined) {
                    session.endSession();
                }
                restartReplicationOnSecondaries(shardRs);
                shardRs.awaitReplication();
            }
        });
    });
}

runTests({
    label: "Replica set (mongod path)",
    makeFixture: () => {
        const rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
        return rst;
    },
    teardown: (rst) => rst.stopSet(),
    getRouter: (rst) => rst.getPrimary(),
    getShardRs: (rst) => rst,
    getMetricsConn: (rst) => rst.getPrimary(),
});

runTests({
    label: "Sharded cluster (mongos path)",
    makeFixture: () => new ShardingTest({shards: 1, rs: {nodes: 2}}),
    teardown: (st) => st.stop(),
    getRouter: (st) => st.s,
    getShardRs: (st) => st.rs0,
    getMetricsConn: (st) => st.rs0.getPrimary(),
});

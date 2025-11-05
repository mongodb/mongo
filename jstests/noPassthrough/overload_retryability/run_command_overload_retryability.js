/**
 * Test that the ingress request rate limiter works correctly and exposes the right metrics.
 * @tags: [requires_fcv_80]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const rs0Name = "rs0";
const shardId = `${jsTest.name()}-${rs0Name}`;
const kCollName = `${jsTest.name()}_coll`;
const kDbName = `${jsTest.name()}_db`;

const kFailCommandOff = {
    configureFailPoint: "failCommand",
    mode: "off",
};

function shardingStatisticsDifference(stats1, stats2) {
    return {
        numOperationsAttempted: stats1.numOperationsAttempted - stats2.numOperationsAttempted,
        numOperationsRetriedAtLeastOnceDueToOverload:
            stats1.numOperationsRetriedAtLeastOnceDueToOverload - stats2.numOperationsRetriedAtLeastOnceDueToOverload,
        numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded:
            stats1.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded -
            stats2.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded,
        numRetriesDueToOverloadAttempted:
            stats1.numRetriesDueToOverloadAttempted - stats2.numRetriesDueToOverloadAttempted,
        numOverloadErrorsReceived: stats1.numOverloadErrorsReceived - stats2.numOverloadErrorsReceived,
        totalBackoffTimeMillis: stats1.totalBackoffTimeMillis - stats2.totalBackoffTimeMillis,
        retryBudgetTokenBucketBalance: stats1.retryBudgetTokenBucketBalance - stats2.retryBudgetTokenBucketBalance,
    };
}

function runTestSingleCommand(execTest, command, conn, rs0) {
    const db = conn.getDB(kDbName);
    const rsAdmin = rs0.getDB("admin");

    const initialShardStats = db.serverStatus().shardingStatistics.shards[shardId];

    const commands = Array.isArray(command) ? command : [command];

    assert.commandWorked(
        rs0.getDB("admin").adminCommand({
            configureFailPoint: "failCommand",
            mode: {times: 3},
            data: {
                errorCode: ErrorCodes.IngressRequestRateLimitExceeded,
                failCommands: commands,
                failInternalCommands: true,
                errorLabels: ["SystemOverloadedError", "RetryableError"],
            },
        }),
    );
    execTest(db);
    assert.commandWorked(rs0.getDB("admin").adminCommand(kFailCommandOff));

    const finalShardStats = db.serverStatus().shardingStatistics.shards[shardId];
    const shardStats = shardingStatisticsDifference(finalShardStats, initialShardStats);

    assert.eq(shardStats.numOperationsAttempted, 1);
    assert.eq(shardStats.numOperationsRetriedAtLeastOnceDueToOverload, 1);
    assert.eq(shardStats.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded, 1);
    assert.eq(shardStats.numRetriesDueToOverloadAttempted, 3);
    assert.eq(shardStats.numOverloadErrorsReceived, 3);
    assert.gt(shardStats.totalBackoffTimeMillis, 0);
    assert.lt(shardStats.retryBudgetTokenBucketBalance, 1000);
}

function testInsert(db) {
    assert.commandWorked(db.runCommand({insert: kCollName, documents: [{name: "test0"}]}));
}

function testFind(db) {
    assert.commandWorked(db.runCommand({find: kCollName, filter: {name: "test0"}}));
}

function testDistinct(db) {
    assert.commandWorked(db.runCommand({distinct: kCollName, key: "name"}));
}

function testCount(db) {
    assert.commandWorked(db.runCommand({count: kCollName}));
}

function testCreateIndex(db) {
    assert.commandWorked(
        db.runCommand({
            createIndexes: kCollName,
            indexes: [
                {
                    name: "name_1",
                    key: {key: 1},
                },
            ],
        }),
    );
}

function testDropIndex(db) {
    assert.commandWorked(db.runCommand({dropIndexes: kCollName, index: {name: 1}}));
}

function testListIndexes(db) {
    assert.commandWorked(db.runCommand({listIndexes: kCollName}));
}

function testShardCollection(db) {
    assert.commandWorked(
        db.adminCommand({
            shardCollection: `${kDbName}.${kCollName}`,
            key: {key: 1},
        }),
    );
}

const kStartupParams = {
    "failpoint.failCommand": tojson({
        mode: "off",
    }),
    "defaultClientBaseBackoffMillis": 10,
    "defaultClientMaxBackoffMillis": 1000,
};

/**
 * Runs a test for the ingress admission rate limiter using sharding.
 */
function runTestSharded() {
    const st = new ShardingTest({
        mongos: 1,
        shards: {
            [rs0Name]: {nodes: 3},
        },
        other: {
            mongosOptions: {
                setParameter: {
                    ...kStartupParams,
                },
            },
            rsOptions: {
                setParameter: {
                    ...kStartupParams,
                },
            },
        },
    });

    const rs0Primary = st.rs0.getPrimary();

    const db = st.s.getDB(`${jsTest.name()}_db`);

    // Warmup coll. Otherwise, we'll see more requests in the stats than we should read.
    assert.commandWorked(
        db[kCollName].insertMany([
            {name: "test0", key: 0},
            {name: "test1", key: 1},
        ]),
    );

    runTestSingleCommand(testCount, "count", st.s, rs0Primary);
    runTestSingleCommand(testDistinct, "distinct", st.s, rs0Primary);
    runTestSingleCommand(testFind, "find", st.s, rs0Primary);
    runTestSingleCommand(testInsert, "insert", st.s, rs0Primary);

    // As dropIndexes seem to run on mongod and not mongos, skip dropIndexes from this test.
    runTestSingleCommand(testCreateIndex, "createIndexes", st.s, rs0Primary);
    runTestSingleCommand(testListIndexes, "listIndexes", st.s, rs0Primary);

    st.stop();
}

runTestSharded();

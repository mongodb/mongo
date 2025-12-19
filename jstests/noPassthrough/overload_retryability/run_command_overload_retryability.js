/**
 * Test that the ingress request rate limiter works correctly and exposes the right metrics.
 * @tags: [requires_fcv_80]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const rs0Name = "rs0";
const shardId = `${jsTest.name()}-${rs0Name}`;
const kCollName = `${jsTest.name()}_coll`;
const kDbName = `${jsTest.name()}_db`;

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
        numRetriesRetargetedDueToOverload:
            stats1.numRetriesRetargetedDueToOverload - stats2.numRetriesRetargetedDueToOverload,
        numOverloadErrorsReceived: stats1.numOverloadErrorsReceived - stats2.numOverloadErrorsReceived,
        totalBackoffTimeMillis: stats1.totalBackoffTimeMillis - stats2.totalBackoffTimeMillis,
        retryBudgetTokenBucketBalance: stats1.retryBudgetTokenBucketBalance - stats2.retryBudgetTokenBucketBalance,
    };
}

function enableFailCommand(conn, command, times) {
    const commands = Array.isArray(command) ? command : [command];

    assert.commandWorked(
        conn.adminCommand({
            configureFailPoint: "failCommand",
            mode: {times},
            data: {
                errorCode: ErrorCodes.IngressRequestRateLimitExceeded,
                failCommands: commands,
                failInternalCommands: true,
                errorLabels: ["SystemOverloadedError", "RetryableError"],
            },
        }),
    );
}

function disableFailCommand(conn) {
    assert.commandWorked(
        conn.adminCommand({
            configureFailPoint: "failCommand",
            mode: "off",
        }),
    );
}

function getShardingStats(conn) {
    return conn.getDB("admin").serverStatus().shardingStatistics.shards[shardId];
}

function runTestOnlyPrimaryFails(commandName, command, readPref, mongos, shard) {
    jsTestLog("Running primary-failure test with command '" + commandName + "' and read preference '" + readPref + "'");

    // Iniitial request must target the primary in these tests.
    assert(!readPref || ["primary", "primaryPreferred"].includes(readPref));

    const kNumFailures = 3;

    const shardPrimary = shard.getPrimary();
    const initialShardStats = getShardingStats(mongos);

    enableFailCommand(shardPrimary, commandName, kNumFailures);

    const cmd = {
        ...command,
        "$readPreference": readPref ? {mode: readPref} : command["$readPreference"],
    };
    jsTestLog("Executing command: ", cmd);
    assert.commandWorked(mongos.getDB(kDbName).runCommand(cmd));

    disableFailCommand(shardPrimary);

    const finalShardStats = getShardingStats(mongos);
    const shardStatsDiff = shardingStatisticsDifference(finalShardStats, initialShardStats);

    assert.eq(shardStatsDiff.numOperationsAttempted, 1);
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverload, 1);
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded, 1);
    assert.gt(shardStatsDiff.totalBackoffTimeMillis, 0);

    if (!readPref || readPref == "primary") {
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, kNumFailures);
        assert.eq(shardStatsDiff.numOverloadErrorsReceived, kNumFailures);
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, 0);
    } else if (readPref == "primaryPreferred") {
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, 1);
        assert.eq(shardStatsDiff.numOverloadErrorsReceived, 1);
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, 1);
    }
}

function runTestAllNodesFail(commandName, command, readPref, mongos, shard) {
    jsTestLog(
        "Running all-nodes-failure test with command '" + commandName + "' and read preference '" + readPref + "'",
    );
    const initialShardStats = getShardingStats(mongos);

    const kNumFailures = 1;
    for (let node of shard.nodes) {
        enableFailCommand(node, commandName, kNumFailures);
    }

    const cmd = {
        ...command,
        "$readPreference": readPref ? {mode: readPref} : command["$readPreference"],
    };
    jsTestLog("Executing command: ", cmd);
    assert.commandWorked(mongos.getDB(kDbName).runCommand(cmd));

    for (let node of shard.nodes) {
        disableFailCommand(node);
    }

    const finalShardStats = getShardingStats(mongos);
    const shardStatsDiff = shardingStatisticsDifference(finalShardStats, initialShardStats);

    jsTestLog(shardStatsDiff);

    assert.eq(shardStatsDiff.numOperationsAttempted, 1);
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverload, 1);
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded, 1);
    assert.gt(shardStatsDiff.totalBackoffTimeMillis, 0);

    if (!readPref || readPref === "primary") {
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, kNumFailures);
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, 0);
        assert.eq(shardStatsDiff.numOverloadErrorsReceived, kNumFailures);
    } else if (readPref === "secondary") {
        // If we can only retry on secondaries, we retry on each secondary once.
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, shard.nodes.length - 1);
        // One of these retries will be on a secondary we already attempted, since we will have run out of other secondaries to try.
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, shard.nodes.length - 1 - 1);
        // Each secondary will have returned an overloaded error exactly once.
        assert.eq(shardStatsDiff.numOverloadErrorsReceived, shard.nodes.length - 1);
    } else {
        // For all other read preferences, we retry once on each node.
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, shard.nodes.length);
        // Each retry will avoid a previously selected server except for the last one, which must choose an already deprioritized secondary.
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, shard.nodes.length - 1);
        // Each node will have returned an overloaded error exactly once.
        assert.eq(shardStatsDiff.numOverloadErrorsReceived, shard.nodes.length);
    }
}

const kStartupParams = {
    "failpoint.failCommand": tojson({
        mode: "off",
    }),
    "failpoint.returnMaxBackoffDelay": tojson({
        mode: "alwaysOn",
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

    const shard = st.rs0;

    // Warmup coll. Otherwise, we'll see more requests in the stats than we should read.
    const db = st.s.getDB(`${jsTest.name()}_db`);
    assert.commandWorked(
        db[kCollName].insertMany([
            {name: "test0", key: 0},
            {name: "test1", key: 1},
        ]),
    );

    const readPrefs = [null, "primary", "secondary", "secondaryPreferred", "nearest"];

    const readCommands = [
        ["find", {find: kCollName, filter: {name: "test0"}}],
        ["aggregate", {aggregate: kCollName, pipeline: [{"$match": {name: "test0"}}], cursor: {}}],
        ["count", {count: kCollName}],
        ["distinct", {distinct: kCollName, key: "name"}],
        ["listIndexes", {listIndexes: kCollName}],
    ];

    const writeCommands = [
        ["insert", {insert: kCollName, documents: [{name: "test0"}]}],
        ["update", {update: kCollName, updates: [{q: {"foo": "barr"}, u: {"$set": {"x": 2}}}]}],
        ["aggregate", {aggregate: kCollName, pipeline: [{"$match": {"foo": "bar"}}, {"$out": "newColl"}], cursor: {}}],
        [
            "createIndexes",
            {
                createIndexes: kCollName,
                indexes: [
                    {
                        name: "name_1",
                        key: {key: 1},
                    },
                ],
            },
        ],
    ];

    jsTestLog("Testing retry behavior when every node in the shard fails.");
    for (let readPref of readPrefs) {
        for (let readCommand of readCommands) {
            runTestAllNodesFail(readCommand[0], readCommand[1], readPref, st.s, shard);
        }
    }
    for (let writeCommand of writeCommands) {
        runTestAllNodesFail(writeCommand[0], writeCommand[1], null, st.s, shard);
    }

    jsTestLog("Testing retry behavior when only the primary fails");
    for (let readPref of [null, "primary", "primaryPreferred"]) {
        for (let readCommand of readCommands) {
            runTestOnlyPrimaryFails(readCommand[0], readCommand[1], readPref, st.s, shard);
        }
    }
    for (let writeCommand of writeCommands) {
        runTestOnlyPrimaryFails(writeCommand[0], writeCommand[1], null, st.s, shard);
    }

    st.stop();
}

runTestSharded();

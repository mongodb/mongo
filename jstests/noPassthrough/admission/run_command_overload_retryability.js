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
            stats1.numOperationsRetriedAtLeastOnceDueToOverload -
            stats2.numOperationsRetriedAtLeastOnceDueToOverload,
        numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded:
            stats1.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded -
            stats2.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded,
        numRetriesDueToOverloadAttempted:
            stats1.numRetriesDueToOverloadAttempted - stats2.numRetriesDueToOverloadAttempted,
        numRetriesRetargetedDueToOverload:
            stats1.numRetriesRetargetedDueToOverload - stats2.numRetriesRetargetedDueToOverload,
        numOverloadErrorsReceived:
            stats1.numOverloadErrorsReceived - stats2.numOverloadErrorsReceived,
        totalBackoffTimeMillis: stats1.totalBackoffTimeMillis - stats2.totalBackoffTimeMillis,
        retryBudgetTokenBucketBalance:
            stats1.retryBudgetTokenBucketBalance - stats2.retryBudgetTokenBucketBalance,
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
                errorLabels: ["SystemOverloadedError", "RetryableError", "NoWritesPerformed"],
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

function runTestOnlyPrimaryFails(
    commandName,
    command,
    readPref,
    mongos,
    shard,
    overloadRetargeting,
) {
    jsTestLog(
        "Running primary-failure test with command '" +
            commandName +
            "', read preference '" +
            readPref +
            "', retargeting: " +
            overloadRetargeting,
    );

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

    // Without overloadRetargeting, all commands are routed to the primary.
    if (!readPref || readPref == "primary" || !overloadRetargeting) {
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, kNumFailures);
        assert.eq(shardStatsDiff.numOverloadErrorsReceived, kNumFailures);
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, 0);
    } else if (readPref == "primaryPreferred" && overloadRetargeting) {
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, 1);
        assert.eq(shardStatsDiff.numOverloadErrorsReceived, 1);
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, 1);
    }
}

function runTestAllNodesFail(commandName, command, readPref, mongos, shard, overloadRetargeting) {
    jsTestLog(
        "Running all-nodes-failure test with command '" +
            commandName +
            "', read preference '" +
            readPref +
            "', retargeting: " +
            overloadRetargeting,
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
    // Each error should be associated with a retry.
    assert.eq(
        shardStatsDiff.numOverloadErrorsReceived,
        shardStatsDiff.numRetriesDueToOverloadAttempted,
    );

    if (
        !readPref ||
        readPref === "primary" ||
        (readPref === "primaryPreferred" && !overloadRetargeting)
    ) {
        // All retries will be performed against the same primary.
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, kNumFailures);
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, 0);
    } else if (readPref === "secondary" && overloadRetargeting) {
        // If we can only retry on secondaries, we retry on each secondary once.
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, shard.nodes.length - 1);
        // One of these retries will be on a secondary we already attempted, since we will have run out of
        // other secondaries to try.
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, shard.nodes.length - 1 - 1);
    } else if (overloadRetargeting) {
        // All other read preferences can select all nodes, so with retargeting we retry once on each node.
        assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, shard.nodes.length);
        // Each retry will avoid a previously selected server except for the last one, which must choose an already
        // deprioritized secondary.
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, shard.nodes.length - 1);
    } else {
        // Without retargeting, we retry at least kNumFailures times, which only occurs if we happen to
        // reselect the same server.
        assert.gte(shardStatsDiff.numRetriesDueToOverloadAttempted, kNumFailures);
        assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, 0);
    }
}

function runTestZeroRetries(commandName, command, mongos, shard) {
    jsTestLog("Running zero-retries test with command '" + commandName + "'");

    const shardPrimary = shard.getPrimary();
    const initialShardStats = getShardingStats(mongos);

    const origMaxRetryAttempts = assert.commandWorked(
        mongos.adminCommand({getParameter: 1, defaultClientMaxRetryAttempts: 1}),
    ).defaultClientMaxRetryAttempts;

    assert.commandWorked(mongos.adminCommand({setParameter: 1, defaultClientMaxRetryAttempts: 0}));
    enableFailCommand(shardPrimary, commandName, 1);

    try {
        const result = mongos.getDB(kDbName).runCommand(command);
        assert.commandFailedWithCode(result, ErrorCodes.IngressRequestRateLimitExceeded);

        // TODO SERVER-128710 Revisit if RetryableError should be reapplied by mongos
        for (const label of ["SystemOverloadedError", "RetryableError", "NoWritesPerformed"]) {
            assert(result.errorLabels?.includes(label), `Expected error label '${label}'`, {
                result,
            });
        }
    } finally {
        disableFailCommand(shardPrimary);
        assert.commandWorked(
            mongos.adminCommand({
                setParameter: 1,
                defaultClientMaxRetryAttempts: origMaxRetryAttempts,
            }),
        );
    }

    const finalShardStats = getShardingStats(mongos);
    const shardStatsDiff = shardingStatisticsDifference(finalShardStats, initialShardStats);

    assert.eq(shardStatsDiff.numOperationsAttempted, 1);
    assert.eq(shardStatsDiff.numOverloadErrorsReceived, 1);
    assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, 0);
    // The counter is incremented on the first overload error for an operation, even when no
    // retry is performed (budget = 0).
    // TODO SERVER-129657 Investigate numOperationsRetriedAtLeastOnceDueToOverload logic
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverload, 1);
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded, 0);
    assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, 0);
}

function runTestRetryCapExhausted(commandName, command, mongos, shard) {
    jsTestLog("Running retry-cap-exhausted test with command '" + commandName + "'");

    // Call primary, then retry twice on primary
    const retryAttempts = 2;
    const shardPrimary = shard.getPrimary();
    const initialShardStats = getShardingStats(mongos);

    const {
        defaultClientMaxRetryAttempts: origMaxRetryAttempts,
        defaultClientBaseBackoffMillis: origBaseBackoffMillis,
        defaultClientMaxBackoffMillis: origMaxBackoffMillis,
    } = assert.commandWorked(
        mongos.adminCommand({
            getParameter: 1,
            defaultClientMaxRetryAttempts: 1,
            defaultClientBaseBackoffMillis: 1,
            defaultClientMaxBackoffMillis: 1,
        }),
    );

    assert.commandWorked(
        mongos.adminCommand({
            setParameter: 1,
            defaultClientMaxRetryAttempts: retryAttempts,
            defaultClientBaseBackoffMillis: 0,
            defaultClientMaxBackoffMillis: 0,
        }),
    );
    enableFailCommand(shardPrimary, commandName, retryAttempts + 1);

    try {
        const result = mongos.getDB(kDbName).runCommand(command);
        assert.commandFailedWithCode(result, ErrorCodes.IngressRequestRateLimitExceeded);

        // TODO SERVER-128710 Revisit if RetryableError should be reapplied by mongos
        for (const label of ["SystemOverloadedError", "RetryableError", "NoWritesPerformed"]) {
            assert(result.errorLabels?.includes(label), `Expected error label '${label}'`, {
                result,
            });
        }
    } finally {
        disableFailCommand(shardPrimary);
        assert.commandWorked(
            mongos.adminCommand({
                setParameter: 1,
                defaultClientMaxRetryAttempts: origMaxRetryAttempts,
                defaultClientBaseBackoffMillis: origBaseBackoffMillis,
                defaultClientMaxBackoffMillis: origMaxBackoffMillis,
            }),
        );
    }

    const finalShardStats = getShardingStats(mongos);
    const shardStatsDiff = shardingStatisticsDifference(finalShardStats, initialShardStats);

    assert.eq(shardStatsDiff.numOperationsAttempted, 1);
    assert.eq(shardStatsDiff.numOverloadErrorsReceived, retryAttempts + 1);
    assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, retryAttempts);
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverload, 1);
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded, 0);
    assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, 0);
}

function runTestRetryCapExhaustedWithRetargeting(commandName, command, readPref, mongos, shard) {
    jsTestLog(
        "Running retry-cap-exhausted with retargeting test with command '" +
            commandName +
            "', read preference '" +
            readPref +
            "'",
    );

    // Call primary, and then retry on each secondary
    const retryAttempts = shard.nodes.length - 1;
    const initialShardStats = getShardingStats(mongos);

    const {
        defaultClientMaxRetryAttempts: origMaxRetryAttempts,
        defaultClientBaseBackoffMillis: origBaseBackoffMillis,
        defaultClientMaxBackoffMillis: origMaxBackoffMillis,
        overloadAwareServerSelectionEnabled: origRetargeting,
    } = assert.commandWorked(
        mongos.adminCommand({
            getParameter: 1,
            defaultClientMaxRetryAttempts: 1,
            defaultClientBaseBackoffMillis: 1,
            defaultClientMaxBackoffMillis: 1,
            overloadAwareServerSelectionEnabled: 1,
        }),
    );

    assert.commandWorked(
        mongos.adminCommand({
            setParameter: 1,
            defaultClientMaxRetryAttempts: retryAttempts,
            defaultClientBaseBackoffMillis: 0,
            defaultClientMaxBackoffMillis: 0,
            overloadAwareServerSelectionEnabled: true,
        }),
    );

    // Fail each node once. retryAttempts == shard.nodes.length - 1, so each retry targets a
    // fresh node until the cap is hit with no node revisited.
    for (let node of shard.nodes) {
        enableFailCommand(node, commandName, 1);
    }

    const cmd = {
        ...command,
        "$readPreference": {mode: readPref},
    };
    jsTestLog("Executing command: ", cmd);

    try {
        const result = mongos.getDB(kDbName).runCommand(cmd);
        assert.commandFailedWithCode(result, ErrorCodes.IngressRequestRateLimitExceeded);

        // TODO SERVER-128710 Revisit if RetryableError should be reapplied by mongos
        for (const label of ["SystemOverloadedError", "RetryableError", "NoWritesPerformed"]) {
            assert(result.errorLabels?.includes(label), `Expected error label '${label}'`, {
                result,
            });
        }
    } finally {
        for (let node of shard.nodes) {
            disableFailCommand(node);
        }
        assert.commandWorked(
            mongos.adminCommand({
                setParameter: 1,
                defaultClientMaxRetryAttempts: origMaxRetryAttempts,
                defaultClientBaseBackoffMillis: origBaseBackoffMillis,
                defaultClientMaxBackoffMillis: origMaxBackoffMillis,
                overloadAwareServerSelectionEnabled: origRetargeting,
            }),
        );
    }

    const finalShardStats = getShardingStats(mongos);
    const shardStatsDiff = shardingStatisticsDifference(finalShardStats, initialShardStats);

    assert.eq(shardStatsDiff.numOperationsAttempted, 1);
    assert.eq(shardStatsDiff.numOverloadErrorsReceived, retryAttempts + 1);
    assert.eq(shardStatsDiff.numRetriesDueToOverloadAttempted, retryAttempts);
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverload, 1);
    assert.eq(shardStatsDiff.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded, 0);
    // Every retry reaches a fresh node since retryAttempts == shard.nodes.length - 1.
    assert.eq(shardStatsDiff.numRetriesRetargetedDueToOverload, retryAttempts);
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

    const readPrefs = [
        null,
        "primary",
        "primaryPreferred",
        "secondary",
        "secondaryPreferred",
        "nearest",
    ];

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
        [
            "aggregate",
            {
                aggregate: kCollName,
                pipeline: [{"$match": {"foo": "bar"}}, {"$out": "newColl"}],
                cursor: {},
            },
        ],
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

    for (let overloadRetargeting of [false, true]) {
        jsTestLog(
            "Testing retry behavior when every node in the shard fails. (retargeting: " +
                overloadRetargeting +
                ")",
        );
        st.s.adminCommand({
            setParameter: 1,
            overloadAwareServerSelectionEnabled: overloadRetargeting,
        });
        for (let readPref of readPrefs) {
            for (let readCommand of readCommands) {
                runTestAllNodesFail(
                    readCommand[0],
                    readCommand[1],
                    readPref,
                    st.s,
                    shard,
                    overloadRetargeting,
                );
            }
        }
        for (let writeCommand of writeCommands) {
            runTestAllNodesFail(writeCommand[0], writeCommand[1], null, st.s, shard);
        }

        jsTestLog("Testing retry behavior when only the primary fails");
        for (let readPref of [null, "primary", "primaryPreferred"]) {
            for (let readCommand of readCommands) {
                runTestOnlyPrimaryFails(
                    readCommand[0],
                    readCommand[1],
                    readPref,
                    st.s,
                    shard,
                    overloadRetargeting,
                );
            }
        }
        for (let writeCommand of writeCommands) {
            runTestOnlyPrimaryFails(
                writeCommand[0],
                writeCommand[1],
                null,
                st.s,
                shard,
                overloadRetargeting,
            );
        }
    }

    jsTestLog("Testing retry budget exhaustion scenarios");
    const findCommand = readCommands[0];
    runTestZeroRetries(findCommand[0], findCommand[1], st.s, shard);
    runTestRetryCapExhausted(findCommand[0], findCommand[1], st.s, shard);
    runTestRetryCapExhaustedWithRetargeting(findCommand[0], findCommand[1], "nearest", st.s, shard);

    st.stop();
}

runTestSharded();

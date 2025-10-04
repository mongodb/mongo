/**
 * Tests that read preference metrics are correctly collected on mongod. Note that the shell used in
 * testing does not perform any server selection logic, and so this test just ensures that if a
 * mongod receives a request with $readPreference attached, it will record the correct metrics.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "dbName";
const collName = "collName";

function getReadPreferenceMetrics(conn) {
    const serverStatus = assert.commandWorked(conn.getDB("admin").runCommand({serverStatus: 1}));
    assert(
        serverStatus.process.startsWith("mongod"),
        "Server status 'process' field does not start with 'mongod'. process: " + serverStatus.process,
    );
    assert(
        serverStatus.hasOwnProperty("readPreferenceCounters"),
        "Server status object is missing 'readPreferenceCounters' field",
    );
    return serverStatus.readPreferenceCounters;
}

// Verify the correct 'readPref' metric was incremented by checking the serverStatus on 'conn'
// before and after the read operation.
function verifyMetricIncrement(conn, readPref, executedOn, tagged) {
    const preMetrics = getReadPreferenceMetrics(conn);

    const cmd = {count: collName, $readPreference: {mode: readPref}};
    if (tagged) {
        cmd.$readPreference.tags = [{region: "us-east-1"}];
    }

    conn.getDB(dbName).runCommand(cmd);
    const postMetrics = getReadPreferenceMetrics(conn);

    const expectedCount = preMetrics[executedOn][readPref].external + 1;
    const count = postMetrics[executedOn][readPref].external;

    assert(
        expectedCount == count,
        `Actual count ${count} did not equal expected count ${expectedCount} for readPreference ${readPref}.`,
    );

    if (tagged) {
        const expectedTaggedCount = preMetrics[executedOn].tagged.external + 1;
        const taggedCount = postMetrics[executedOn].tagged.external;
        assert(
            expectedTaggedCount == taggedCount,
            `Actual tagged count ${taggedCount} did not equal to expected tagged count ${
                expectedTaggedCount
            } for read preference ${readPref}.`,
        );
    }
}

function runTest(fixture) {
    const primary = fixture.getPrimary();
    const secondary = fixture.getSecondary();

    const preferences = ["primary", "primaryPreferred", "secondary", "secondaryPreferred", "nearest"];

    for (const readPref of preferences) {
        verifyMetricIncrement(primary, readPref, "executedOnPrimary");
        if (readPref != "primary") {
            // For the tagged test on the primary and both tests on the secondary, we skip the
            // primary read preference case. This is because this read preference does not support
            // tag sets, and the command will fail on the secondary before we increment any
            // metrics.
            verifyMetricIncrement(primary, readPref, "executedOnPrimary", true /* tagged */);
            verifyMetricIncrement(secondary, readPref, "executedOnSecondary");
            verifyMetricIncrement(secondary, readPref, "executedOnSecondary", true /* tagged */);
        }
    }
}

// Test that a standalone mongod omits read preference metrics from its serverStatus output.
const standalone = MongoRunner.runMongod({});
let serverStatus = assert.commandWorked(standalone.getDB("admin").runCommand({serverStatus: 1}));
assert(!serverStatus.hasOwnProperty("readPreferenceCounters"), tojson(serverStatus));
MongoRunner.stopMongod(standalone);

// Test that replica set nodes tracks metrics around read preference.
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

jsTestLog("Testing against replica set");
runTest(rst);

rst.stopSet();

// Test that mongos omits metrics around read preference, and shard servers include them.
const st = new ShardingTest({shards: 1, rs: {nodes: 2}});

serverStatus = assert.commandWorked(st.s.getDB("admin").runCommand({serverStatus: 1}));
assert(serverStatus.process.startsWith("mongos"), tojson(serverStatus));
assert(!serverStatus.hasOwnProperty("readPreferenceCounters"), tojson(serverStatus));

jsTestLog("Testing against sharded cluster");
runTest(st.rs0);

st.stop();

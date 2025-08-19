/**
 * Tests that mongod and mongos can start with every extension in
 * bazel-bin/install-extensions/lib/ successfully loaded.
 *
 * If any .so fails to load at startup, the server will crash and this
 * test will not even reach the assertion below.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {isLinux} from "jstests/libs/os_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Extensions are only supported on Linux platforms, so skip this test on other operating systems
if (!isLinux()) {
    quit();
}

const pathToExtensionFoo = MongoRunner.getExtensionPath("libfoo_mongo_extension.so");
const extOpts = {
    loadExtensions: pathToExtensionFoo,
    setParameter: {featureFlagExtensionsAPI: true}
};

// Helper to verify the three log-based conditions on any connection
function assertExtensionsLoaded(conn) {
    const res = assert.commandWorked(conn.getDB("admin").runCommand({getLog: "global"}));
    const lines = res.log;
    const extensionLogs = lines.filter(l => l.includes("EXTENSION"));

    const loaded = extensionLogs.filter(l => l.includes("Loading extension"));
    const success = extensionLogs.filter(l => l.includes("Successfully loaded extension"));
    const errors = extensionLogs.filter(l => l.includes("Error loading extension"));

    assert.gt(loaded.length, 0, 'Did not see "Loading extension" in ' + conn + " log");
    assert.gt(success.length, 0, 'Did not see "Successfully loaded extension" in ' + conn + " log");
    assert.eq(errors.length, 0, 'Saw "Error loading extension" lines:\n' + errors.join("\n"));
}

// 1) Standalone mongod
const conn = MongoRunner.runMongod(extOpts);
let coll = conn.getCollection('test.foo');
assert.commandWorked(coll.insert({ok: 1}));
assert.eq(1, coll.countDocuments({ok: 1}));
assertExtensionsLoaded(conn);
MongoRunner.stopMongod(conn);

// 2) Sharded cluster’s mongos
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2},
    mongos: 1,
    config: 1,
    mongosOptions: extOpts,
    configOptions: extOpts,
    rsOptions: extOpts
});

coll = st.s0.getCollection('test.foo');
assert.commandWorked(coll.insert({ok: 1}));
assert.eq(1, coll.countDocuments({ok: 1}));
assertExtensionsLoaded(st.s0);

st.stop();

print("Both mongod and mongos loaded extensions successfully.");

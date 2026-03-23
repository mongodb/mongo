/*
 * Test to validate checkMetadataConsistency uses snapshot read concern.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, name: jsTestName(), other: {enableBalancer: false}});

const dbName = jsTestName();
const db = st.s.getDB(dbName);
const collName = "check_metadata_consistency";
const coll = db[collName];

assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {account: 1}}));

jsTest.log.info("Log all queries");
for (const node of st.configRS.nodes) {
    assert.commandWorked(node.getDB("config").setProfilingLevel(0, {slowms: 0}));
}

jsTest.log.info("Run checkMetadataConsistency");
assert.commandWorked(db.adminCommand({checkMetadataConsistency: 1}));

for (const node of st.configRS.nodes) {
    assert.commandWorked(node.getDB("config").setProfilingLevel(0, {slowms: 0}));
}

jsTest.log.info("Checking logs for snapshot read concern");

// Make sure we've used snapshot read concern for the query on config.chunks
// `"readConcern":{"level":"snapshot"` should appear twice, once from the passed in command and once
// from the actual readConcern used. Check all nodes to account for stepdowns.
assert(
    st.configRS.nodes.some((node) =>
        checkLog.checkContainsOnce(node, /"config.chunks"(.*"readConcern":{"level":"snapshot"){2}/),
    ),
    "The query on config.chunks should use snapshot read concern",
);

st.stop();

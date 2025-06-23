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

jsTest.log("Log all queries");
assert.commandWorked(st.configRS.getPrimary().getDB("config").setProfilingLevel(0, {slowms: 0}));

jsTest.log("Run checkMetadataConsistency");
assert.commandWorked(db.adminCommand({checkMetadataConsistency: 1}));

assert.commandWorked(st.configRS.getPrimary().getDB("config").setProfilingLevel(0, {slowms: 100}));

jsTest.log("Checking logs for snapshot read concern");

// Make sure we've used snapshot read concern for the query on config.chunks
// `"readConcern":{"level":"snapshot"` should appear twice, once from the passed in command and once
// from the actual readConcern used.
assert(checkLog.checkContainsOnce(st.configRS.getPrimary(),
                                  /"config.chunks"(.*"readConcern":{"level":"snapshot"){2}/),
       "The query on config.chunks should use snapshot read concern");

st.stop();

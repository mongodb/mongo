/*
 * Creating a collection and coordinating a multi update both require acquiring the DDL lock. Test
 * to make sure we do not hang when we attempt this.
 * @tags: [
 *  requires_fcv_80
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const numShards = 2;
const st = new ShardingTest({shards: numShards});

assert.commandWorked(st.s.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}}}));
assert.commandWorked(st.s.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));

const db = st.s.getDB("testDb");

// Test with batched write.
assert.commandWorked(db.testColl.update({x: 1}, {$set: {x: 2}}, {upsert: true, multi: true}));

// Drop database between tests to ensure the collection has to be recreated.
assert.commandWorked(db.dropDatabase());

// Test with bulk write.
assert.commandWorked(
    db.adminCommand({
        bulkWrite: 1,
        ops: [{update: 0, filter: {x: 1}, multi: true, updateMods: {$set: {x: 2}}, upsert: true}],
        nsInfo: [{ns: "testDb.testColl"}],
    }),
);

st.stop();

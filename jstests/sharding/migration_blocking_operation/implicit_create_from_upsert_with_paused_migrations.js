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

assert.commandWorked(
    st.s.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}}}));
assert.commandWorked(st.s.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));

const db = st.s.getDB("testDb");
assert.commandWorked(db.testColl.update({x: 1}, {$set: {x: 2}}, {upsert: true, multi: true}));

st.stop();

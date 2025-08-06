/**
 * Test maxTimeMS configured against sharded cluster, and large amount of time spent in fist batch.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const findComment = jsTest.name() + "_comment";
const dbName = "dx";
const collName = "data";

const st = new ShardingTest({shards: 1});
const db = st.s.getDB(dbName);

const coll = db[collName];
coll.drop();
coll.insertMany([{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}, {_id: 3, a: 4}]);

// Wait for 20 * 4 = 80ms for first batch of find command.
assert.commandWorked(
    st.shard0.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
const failPoint =
    configureFailPoint(st.shard0, "setPreYieldWait", {waitForMillis: 20, comment: findComment});

// Find command with sorting, which will take 80ms to return the first batch.
// SERVER-108581 had a bug to count the first batch execution time twice, so setting maxTimeMS to
// 159ms.
const res = coll.find({}).maxTimeMS(159).comment(findComment).batchSize(1).sort({'a': 1, _id: 1});
assert(res.hasNext());
assert.eq(1, res.next().a, "Expected first document to have 'a' equal to 1");

// Turn off the fail point to allow getMore to complete.
failPoint.off();
assert.eq(3, res.toArray().length, "Expected 3 more documents after the first one");

st.stop();

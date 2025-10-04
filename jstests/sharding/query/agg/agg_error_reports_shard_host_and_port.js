// Tests that an aggregation error which occurs on a sharded collection will send an error message
// containing the host and port of the shard where the error occurred.
import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const mongosDb = st.s.getDB(jsTestName());
const coll = mongosDb.getCollection("foo");

// Enable sharding on the test DB and ensure its primary is shard 0.
assert.commandWorked(mongosDb.adminCommand({enableSharding: mongosDb.getName(), primaryShard: st.rs0.getURL()}));

// Shard the collection.
coll.drop();
st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 1});

assert.commandWorked(coll.insert({_id: 0}));

// Run an aggregation which will fail on shard 1, and verify that the error message contains
// the host and port of the shard that failed.
// We need to be careful here to involve some data in the computation that is actually
// sent to the shard before failing (i.e. "$_id") so that mongos doesn't short-curcuit and
// fail during optimization.
const pipe = [{$project: {a: {$divide: ["$_id", 0]}}}];
const divideByZeroErrorCodes = [16608, 4848401, ErrorCodes.BadValue];

assertErrCodeAndErrMsgContains(coll, pipe, divideByZeroErrorCodes, st.rs1.getPrimary().host);

st.stop();

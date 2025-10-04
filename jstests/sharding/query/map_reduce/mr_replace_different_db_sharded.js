// Test MapReduce output option replace into different db in a sharded environment.
// @tags: [requires_scripting]
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, mongos: 1});

const sourceDB = st.s.getDB("mr_source_db");
const destDB = st.s.getDB("mr_out_db");
const sourceColl = sourceDB.mr_source_coll;
sourceColl.drop();
assert.commandWorked(st.s.adminCommand({enableSharding: sourceDB.getName(), primaryShard: st.shard0.name}));
assert.commandWorked(st.s.adminCommand({enableSharding: destDB.getName(), primaryShard: st.shard0.name}));
assert.commandWorked(sourceColl.insert({val: 1}));
assert.commandWorked(sourceColl.insert({val: 2}));

assert.commandWorked(sourceColl.createIndex({val: 1}));
assert.commandWorked(st.s.adminCommand({shardCollection: sourceColl.getFullName(), key: {val: 1}}));

assert.eq(2, sourceColl.find().count());
function mapFunc() {
    emit(this.val, 1);
}
function reduceFunc(k, v) {
    return Array.sum(v);
}
const destColl = destDB.mr_out_coll;
destColl.drop();
assert.commandWorked(destColl.insert({val: 2}));
let result = assert.commandWorked(
    sourceDB.runCommand({
        mapReduce: sourceColl.getName(),
        map: mapFunc,
        reduce: reduceFunc,
        out: {replace: destColl.getName(), db: destDB.getName()},
    }),
);
assert.eq(2, destColl.find().count(), result);

// Test that it works when the dbs are on different shards.
destColl.drop();
destDB.dropDatabase();

assert.commandWorked(st.s.adminCommand({enableSharding: destDB.getName(), primaryShard: st.shard1.name}));

result = assert.commandWorked(
    sourceDB.runCommand({
        mapReduce: sourceColl.getName(),
        map: mapFunc,
        reduce: reduceFunc,
        out: {replace: destColl.getName(), db: destDB.getName()},
    }),
);
assert.eq(2, destColl.find().count(), result);

// Test that it works when the dbs are on different shards and the destination collection has an
// index.
destColl.drop();
destDB.createCollection(destColl.getName());
assert.commandWorked(destColl.createIndex({val: 1}, {name: "test_index"}));

result = assert.commandWorked(
    sourceDB.runCommand({
        mapReduce: sourceColl.getName(),
        map: mapFunc,
        reduce: reduceFunc,
        out: {replace: destColl.getName(), db: destDB.getName()},
    }),
);
assert.eq(2, destColl.find().count(), result);
const finalIndexes = assert.commandWorked(destDB.runCommand({"listIndexes": destColl.getName()}));
const finalIndexesArray = new DBCommandCursor(destDB, finalIndexes).toArray();
assert.eq(2, finalIndexesArray.length);
st.stop();

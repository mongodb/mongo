// The map reduce command is deprecated in 5.0.
//
// In this test, we run the map reduce command multiple times.
// We want to make sure that the deprecation warning message is only logged once despite
// the multiple invocations in an effort to not clutter the dev's console.
// More specifically, we expect to only log 1/127 of mapReduce() events.

(function() {
"use strict";
load("jstests/libs/log.js");  // For findMatchingLogLine, findMatchingLogLines

jsTest.log('Test standalone');
const caseInsensitive = {
    collation: {locale: "simple", strength: 2}
};
const standalone = MongoRunner.runMongod({});
const dbName = 'test';
const collName = "test_map_reduce_command_deprecation_messaging";
const db = standalone.getDB(dbName);
const coll = db.getCollection(collName);
const fieldMatcher = {
    msg:
        "The map reduce command is deprecated. For more information, see https://docs.mongodb.com/manual/core/map-reduce/"
};

function mapFunc() {
    emit(this.cust_id, this.amount);
}
function reduceFunc(key, values) {
    return Array.sum(values);
}

coll.drop();
assert.commandWorked(coll.insert({cust_id: "A", amount: 100, status: "B"}));
assert.commandWorked(coll.insert({cust_id: "A", amount: 200, status: "B"}));
assert.commandWorked(coll.insert({cust_id: "B", amount: 50, status: "B"}));

// Assert that deprecation msg is not logged before map reduce command is even run.
var globalLogs = db.adminCommand({getLog: 'global'});
var matchingLogLines = [...findMatchingLogLines(globalLogs.log, fieldMatcher)];
assert.eq(matchingLogLines.length, 0, matchingLogLines);

assert.commandWorked(db.runCommand(
    {mapReduce: collName, map: mapFunc, reduce: reduceFunc, query: {b: 2}, out: "order_totals"}));

assert.commandWorked(coll.insert({cust_id: "B", amount: 50, status: "B"}));

assert.commandWorked(db.runCommand(
    {mapReduce: collName, map: mapFunc, reduce: reduceFunc, query: {b: 2}, out: "order_totals"}));

assert.commandWorked(coll.insert({cust_id: "A", amount: 200, status: "B"}));

assert.commandWorked(db.runCommand(
    {mapReduce: collName, map: mapFunc, reduce: reduceFunc, query: {"B": 2}, out: "order_totals"}));

// Now that we have ran map reduce command, make sure the deprecation message is logged once.
globalLogs = db.adminCommand({getLog: 'global'});
matchingLogLines = [...findMatchingLogLines(globalLogs.log, fieldMatcher)];
assert.eq(matchingLogLines.length, 1, matchingLogLines);
MongoRunner.stopMongod(standalone);

jsTest.log('Test cluster');

const st = new ShardingTest({shards: 2, mongos: 1});

const session = st.s.getDB("test").getMongo().startSession();
const mongosDB = session.getDatabase("test");
const mongosColl = mongosDB.testing;

mongosColl.drop();
assert.commandWorked(mongosDB.createCollection(mongosColl.getName(), caseInsensitive));

assert.commandWorked(mongosColl.insert({cust_id: "A", amount: 100, status: "B"}));
assert.commandWorked(mongosColl.insert({cust_id: "A", amount: 200, status: "B"}));
assert.commandWorked(mongosColl.insert({cust_id: "B", amount: 50, status: "B"}));
assert.commandWorked(mongosColl.insert({cust_id: "A", amount: 10, status: "B"}));
assert.commandWorked(mongosColl.insert({cust_id: "A", amount: 20, status: "B"}));
assert.commandWorked(mongosColl.insert({cust_id: "B", amount: 5, status: "B"}));

assert.commandWorked(st.s0.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);
assert.commandWorked(
    st.s0.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

// Assert that deprecation msg is not logged before map reduce command is even run.
globalLogs = mongosDB.adminCommand({getLog: 'global'});
matchingLogLines = [...findMatchingLogLines(globalLogs.log, fieldMatcher)];
assert.eq(matchingLogLines.length, 0, matchingLogLines);

// Check the logs of the primary shard of the mongos.
globalLogs = st.shard0.getDB("test").adminCommand({getLog: 'global'});
matchingLogLines = [...findMatchingLogLines(globalLogs.log, fieldMatcher)];
assert.eq(matchingLogLines.length, 0, matchingLogLines);

assert.commandWorked(mongosDB.runCommand({
    mapReduce: mongosColl.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    query: {b: 2},
    out: "order_totals"
}));

assert.commandWorked(mongosColl.insert({cust_id: "B", amount: 50, status: "B"}));

assert.commandWorked(mongosDB.runCommand({
    mapReduce: mongosColl.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    query: {b: 2},
    out: "order_totals"
}));

assert.commandWorked(mongosColl.insert({cust_id: "A", amount: 200, status: "B"}));

assert.commandWorked(mongosDB.runCommand({
    mapReduce: mongosColl.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    query: {"B": 2},
    out: "order_totals"
}));

// Now that we have ran map reduce command, make sure the deprecation message is logged once.
globalLogs = mongosDB.adminCommand({getLog: 'global'});
matchingLogLines = [...findMatchingLogLines(globalLogs.log, fieldMatcher)];
assert.eq(matchingLogLines.length, 1, matchingLogLines);

// Check the logs of the primary shard of the mongos.
globalLogs = st.shard0.getDB("test").adminCommand({getLog: 'global'});
matchingLogLines = [...findMatchingLogLines(globalLogs.log, fieldMatcher)];
assert.eq(matchingLogLines.length, 0, matchingLogLines);

st.stop();
})();

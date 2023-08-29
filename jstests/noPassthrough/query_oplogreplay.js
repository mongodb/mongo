// Test oplog queries that can be optimized with oplogReplay.
// @tags: [requires_replication, requires_capped]

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/storage_engine_utils.js");

let replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();
let conn = replSet.getPrimary();
let testDB = conn.getDB("test");
let oplog = conn.getDB("local").oplog.rs;

/**
 * Helper function for making timestamps with the property that if i < j, then makeTS(i) <
 * makeTS(j).
 */
function makeTS(i) {
    return Timestamp(1000, i);
}

function longToTs(i) {
    return Timestamp(i.top, i.bottom);
}

// The first object is just a dummy element in order to make both index and id match in the tests
// and avoid off-by-1 errors
var timestamps = [{}];

for (let i = 1; i <= 100; i++) {
    let res = testDB.runCommand({insert: jsTestName(), documents: [{_id: i, ts: makeTS(i)}]});
    let ts = res.opTime.ts;
    timestamps.push(ts);
    assert.commandWorked(res);
}

const collNs = `test.${jsTestName()}`;

// A $gt query on just the 'ts' field should return the next document after the timestamp.
var cursor = oplog.find({ns: collNs, ts: {$gt: timestamps[20]}});
assert.eq(21, cursor.next().o["_id"]);
assert.eq(22, cursor.next().o["_id"]);

// A $gte query on the 'ts' field should include the timestamp.
cursor = oplog.find({ns: collNs, ts: {$gte: timestamps[20]}});
assert.eq(20, cursor.next().o["_id"]);
assert.eq(21, cursor.next().o["_id"]);

// An $eq query on the 'ts' field should return the single record with the timestamp.
cursor = oplog.find({ns: collNs, ts: {$eq: timestamps[20]}});
assert.eq(20, cursor.next().o["_id"]);
assert(!cursor.hasNext());

// An AND with both a $gt and $lt query on the 'ts' field will correctly return results in
// the proper bounds.
cursor = oplog.find({$and: [{ns: collNs}, {ts: {$lt: timestamps[5]}}, {ts: {$gt: timestamps[1]}}]});
assert.eq(2, cursor.next().o["_id"]);
assert.eq(3, cursor.next().o["_id"]);
assert.eq(4, cursor.next().o["_id"]);
assert(!cursor.hasNext());

// An AND with multiple predicates on the 'ts' field correctly returns results on the
// tightest range.
cursor = oplog.find({
    $and: [
        {ns: collNs},
        {ts: {$gte: timestamps[2]}},
        {ts: {$gt: timestamps[3]}},
        {ts: {$lte: timestamps[7]}},
        {ts: {$lt: timestamps[7]}}
    ]
});
assert.eq(4, cursor.next().o["_id"]);
assert.eq(5, cursor.next().o["_id"]);
assert.eq(6, cursor.next().o["_id"]);
assert(!cursor.hasNext());

// An AND with an $eq predicate in conjunction with other bounds correctly returns one
// result.
cursor = oplog.find({
    $and: [
        {ns: collNs},
        {ts: {$gte: timestamps[1]}},
        {ts: {$gt: timestamps[2]}},
        {ts: {$eq: timestamps[5]}},
        {ts: {$lte: timestamps[8]}},
        {ts: {$lt: timestamps[8]}}
    ]
});
assert.eq(5, cursor.next().o["_id"]);
assert(!cursor.hasNext());

// An $eq query stops scanning after passing the max timestamp.
let res = oplog.find({ns: collNs, ts: {$eq: timestamps[10]}}).explain("executionStats");
assert.commandWorked(res);
// We expect to be able to seek directly to the entry with a 'ts' of 10.
assert.lte(res.executionStats.totalDocsExamined, 2, tojson(res));
let collScanStage = getPlanStage(getWinningPlan(res.queryPlanner), "COLLSCAN");
assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
assert.eq(timestamps[10], longToTs(collScanStage.maxRecord), tojson(res));

// An AND with an $lt predicate stops scanning after passing the max timestamp.
res = oplog.find({$and: [{ns: collNs}, {ts: {$gte: timestamps[1]}}, {ts: {$lt: timestamps[10]}}]})
          .explain("executionStats");
assert.commandWorked(res);
assert.lte(res.executionStats.totalDocsExamined, 11, tojson(res));
collScanStage = getPlanStage(getWinningPlan(res.queryPlanner), "COLLSCAN");
assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
assert.eq(timestamps[10], longToTs(collScanStage.maxRecord), tojson(res));

// An AND with an $lte predicate stops scanning after passing the max timestamp.
res = oplog.find({$and: [{ns: collNs}, {ts: {$gte: timestamps[1]}}, {ts: {$lte: timestamps[10]}}]})
          .explain("executionStats");
assert.commandWorked(res);
assert.lte(res.executionStats.totalDocsExamined, 12, tojson(res));
collScanStage = getPlanStage(getWinningPlan(res.queryPlanner), "COLLSCAN");
assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
assert.eq(timestamps[10], longToTs(collScanStage.maxRecord), tojson(res));

// The max timestamp is respected even when the min timestamp is smaller than the lowest
// timestamp in the collection.
res = oplog.find({$and: [{ns: collNs}, {ts: {$gte: timestamps[0]}}, {ts: {$lte: timestamps[10]}}]})
          .explain("executionStats");
assert.commandWorked(res);
collScanStage = getPlanStage(getWinningPlan(res.queryPlanner), "COLLSCAN");
assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
assert.eq(timestamps[10], longToTs(collScanStage.maxRecord), tojson(res));

// An AND with redundant $eq/$lt/$lte predicates stops scanning after passing the max
// timestamp.
res = oplog
          .find({
              $and: [
                  {ns: collNs},
                  {ts: {$gte: timestamps[0]}},
                  {ts: {$lte: timestamps[10]}},
                  {ts: {$eq: timestamps[5]}},
                  {ts: {$lt: timestamps[20]}}
              ]
          })
          .explain("executionStats");
assert.commandWorked(res);
// We expect to be able to seek directly to the entry with a 'ts' of 5.
collScanStage = getPlanStage(getWinningPlan(res.queryPlanner), "COLLSCAN");
assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
assert.eq(timestamps[5], longToTs(collScanStage.maxRecord), tojson(res));
assert.eq(timestamps[5], longToTs(collScanStage.minRecord), tojson(res));

// An $eq query for a non-existent timestamp scans a single oplog document.
res = oplog.find({ns: collNs, ts: {$eq: makeTS(200)}}).explain("executionStats");
assert.commandWorked(res);
// We expect to be able to seek directly to the end of the oplog.
collScanStage = getPlanStage(getWinningPlan(res.queryPlanner), "COLLSCAN");
assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
assert.eq(makeTS(200), longToTs(collScanStage.maxRecord), tojson(res));

// When the filter matches the last document within the timestamp range, the collection scan
// examines at most one more document.
res = oplog.find({$and: [{ns: collNs}, {ts: {$gte: timestamps[4]}}, {ts: {$lte: timestamps[8]}}]})
          .explain("executionStats");
assert.commandWorked(res);
// We expect to be able to seek directly to the start of the 'ts' range.
collScanStage = getPlanStage(getWinningPlan(res.queryPlanner), "COLLSCAN");
assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
assert.eq(timestamps[8], longToTs(collScanStage.maxRecord), tojson(res));

// A filter with only an upper bound predicate on 'ts' stops scanning after
// passing the max timestamp.
res = oplog.find({ns: collNs, ts: {$lt: timestamps[4]}}).explain("executionStats");
assert.commandWorked(res);
collScanStage = getPlanStage(getWinningPlan(res.queryPlanner), "COLLSCAN");
assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));
assert.eq(timestamps[4], longToTs(collScanStage.maxRecord), tojson(res));

// Oplog replay optimization should work with projection.
res = oplog.find({ns: collNs, ts: {$lte: timestamps[4]}}).projection({op: 0});
while (res.hasNext()) {
    const next = res.next();
    assert(!next.hasOwnProperty('op'));
    assert(next.hasOwnProperty('ts'));
}
res = res.explain("executionStats");
assert.commandWorked(res);

res = oplog.find({ns: collNs, ts: {$gte: timestamps[90]}}).projection({'op': 0});
while (res.hasNext()) {
    const next = res.next();
    assert(!next.hasOwnProperty('op'));
    assert(next.hasOwnProperty('ts'));
}
res = res.explain("executionStats");
assert.commandWorked(res);

// Oplog replay optimization should work with limit.
res = oplog.find({$and: [{ns: collNs}, {ts: {$gte: timestamps[4]}}, {ts: {$lte: timestamps[8]}}]})
          .limit(2)
          .explain("executionStats");
assert.commandWorked(res);
assert.eq(2, res.executionStats.totalDocsExamined);
collScanStage = getPlanStage(res.executionStats.executionStages, "COLLSCAN");
assert.eq(2, collScanStage.nReturned, res);

// A query over both 'ts' and '_id' should only pay attention to the 'ts' field for finding
// the oplog start (SERVER-13566).
cursor = oplog.find({ns: collNs, ts: {$gte: timestamps[20]}, "o._id": 25});
assert.eq(25, cursor.next().o["_id"]);
assert(!cursor.hasNext());

// 'oplogreplay' flag is allowed but ignored on the oplog collection.
assert.commandWorked(oplog.runCommand({find: oplog.getName(), oplogReplay: true}));

// 'oplogreplay' flag is allowed but ignored on capped collections.
const cappedColl = testDB.cappedColl_jstests_query_oplogreplay;
cappedColl.drop();
assert.commandWorked(
    testDB.createCollection(cappedColl.getName(), {capped: true, size: 16 * 1024}));
for (let i = 1; i <= 100; i++) {
    assert.commandWorked(cappedColl.insert({_id: i, ts: makeTS(i)}));
}
res = cappedColl.runCommand(
    {explain: {find: cappedColl.getName(), filter: {ts: {$eq: makeTS(200)}}, oplogReplay: true}});
assert.commandWorked(res);
assert.eq(res.executionStats.totalDocsExamined, 100);

// Ensure oplog replay hack does not work for backward scans.
res = oplog.find({ns: collNs, ts: {$lt: timestamps[4]}})
          .sort({$natural: -1})
          .explain("executionStats");
assert.commandWorked(res);
assert.gte(res.executionStats.totalDocsExamined, 100, tojson(res));
collScanStage = getPlanStage(getWinningPlan(res.queryPlanner), "COLLSCAN");
assert.neq(null, collScanStage, "no collection scan found in explain output: " + tojson(res));

replSet.stopSet();
}());

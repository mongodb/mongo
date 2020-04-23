// Tests bulk inserts to mongos
// @tags: [requires_fcv_44]

(function() {
'use strict';

var st = new ShardingTest({shards: 2, mongos: 2});

var mongos = st.s;
var staleMongos = st.s1;
var admin = mongos.getDB("admin");

var collSh = mongos.getCollection(jsTestName() + ".collSharded");
var collUn = mongos.getCollection(jsTestName() + ".collUnsharded");

jsTest.log('Checking write to config collections...');
assert.commandWorked(admin.TestColl.insert({SingleDoc: 1}));

jsTest.log("Setting up collections...");

assert.commandWorked(admin.runCommand({enableSharding: collSh.getDB() + ""}));
st.ensurePrimaryShard(collSh.getDB() + "", st.shard0.shardName);

assert.commandWorked(admin.runCommand({movePrimary: collUn.getDB() + "", to: st.shard1.shardName}));

printjson(collSh.ensureIndex({ukey: 1}, {unique: true}));
printjson(collUn.ensureIndex({ukey: 1}, {unique: true}));

assert.commandWorked(admin.runCommand({shardCollection: collSh + "", key: {ukey: 1}}));
assert.commandWorked(admin.runCommand({split: collSh + "", middle: {ukey: 0}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: collSh + "", find: {ukey: 0}, to: st.shard0.shardName, _waitForDelete: true}));

var resetColls = function() {
    assert.commandWorked(collSh.remove({}));
    assert.commandWorked(collUn.remove({}));
};

var isDupKeyError = function(err) {
    return /dup key/.test(err + "");
};

jsTest.log("Collections created.");
st.printShardingStatus();

//
// BREAK-ON-ERROR
//

jsTest.log("Bulk insert (no ContinueOnError) to single shard...");

resetColls();
var inserts = [{ukey: 0}, {ukey: 1}];

assert.commandWorked(collSh.insert(inserts));
assert.eq(2, collSh.find().itcount());

assert.commandWorked(collUn.insert(inserts));
assert.eq(2, collUn.find().itcount());

jsTest.log("Bulk insert (no COE) to single shard...");

resetColls();
var inserts = [{ukey: 0}, {hello: "world"}, {ukey: 1}];

assert.commandWorked(collSh.insert(inserts));
assert.eq(3, collSh.find().itcount());

jsTest.log("Bulk insert (no COE) with mongod error...");

resetColls();
var inserts = [{ukey: 0}, {ukey: 0}, {ukey: 1}];

assert.writeError(collSh.insert(inserts));
assert.eq(1, collSh.find().itcount());

assert.writeError(collUn.insert(inserts));
assert.eq(1, collUn.find().itcount());

jsTest.log("Bulk insert (no COE) with mongod error...");

resetColls();
var inserts = [{ukey: 0}, {ukey: 0}, {ukey: 1}, {hello: "world"}];

var res = assert.writeError(collSh.insert(inserts));
assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
assert.eq(1, collSh.find().itcount());

res = assert.writeError(collUn.insert(inserts));
assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
assert.eq(1, collUn.find().itcount());

jsTest.log("Bulk insert (no COE) on second shard...");

resetColls();
var inserts = [{ukey: 0}, {ukey: -1}];

assert.commandWorked(collSh.insert(inserts));
assert.eq(2, collSh.find().itcount());

assert.commandWorked(collUn.insert(inserts));
assert.eq(2, collUn.find().itcount());

jsTest.log("Bulk insert to second shard (no COE) on second shard...");

resetColls();
var inserts = [
    {ukey: 0},
    {ukey: 1},  // switches shards
    {ukey: -1},
    {hello: "world"}
];

assert.commandWorked(collSh.insert(inserts));
assert.eq(4, collSh.find().itcount());

jsTest.log("Bulk insert to second shard (no COE) with mongod error...");

resetColls();
var inserts = [{ukey: 0}, {ukey: 1}, {ukey: -1}, {ukey: -2}, {ukey: -2}];

assert.writeError(collSh.insert(inserts));
assert.eq(4, collSh.find().itcount());

assert.writeError(collUn.insert(inserts));
assert.eq(4, collUn.find().itcount());

jsTest.log("Bulk insert to third shard (no COE) with mongod error...");

resetColls();
var inserts =
    [{ukey: 0}, {ukey: 1}, {ukey: -2}, {ukey: -3}, {ukey: 4}, {ukey: 4}, {hello: "world"}];

res = assert.writeError(collSh.insert(inserts));
assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
assert.eq(5, collSh.find().itcount());

res = assert.writeError(collUn.insert(inserts));
assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
assert.eq(5, collUn.find().itcount());

//
// CONTINUE-ON-ERROR
//

jsTest.log("Bulk insert (yes COE) with mongod error...");

resetColls();
var inserts = [{ukey: 0}, {ukey: 0}, {ukey: 1}];

assert.writeError(collSh.insert(inserts, 1));
assert.eq(2, collSh.find().itcount());

assert.writeError(collUn.insert(inserts, 1));
assert.eq(2, collUn.find().itcount());

jsTest.log("Bulk insert to third shard (yes COE) with mongod error...");

resetColls();
var inserts =
    [{ukey: 0}, {ukey: 1}, {ukey: -2}, {ukey: -3}, {ukey: 4}, {ukey: 4}, {hello: "world"}];

// Extra insert goes through
res = assert.writeError(collSh.insert(inserts, 1));
assert.eq(6, res.nInserted, res.toString());
assert.eq(6, collSh.find().itcount());

res = assert.writeError(collUn.insert(inserts, 1));
assert.eq(6, res.nInserted, res.toString());
assert.eq(6, collUn.find().itcount());

//
// Test when WBL has to be invoked mid-insert
//

jsTest.log("Testing bulk insert (no COE) with WBL...");
resetColls();

var inserts = [{ukey: 1}, {ukey: -1}];

var staleCollSh = staleMongos.getCollection(collSh + "");
assert.eq(null, staleCollSh.findOne(), 'Collections should be empty');

assert.commandWorked(admin.runCommand(
    {moveChunk: collSh + "", find: {ukey: 0}, to: st.shard1.shardName, _waitForDelete: true}));
assert.commandWorked(admin.runCommand(
    {moveChunk: collSh + "", find: {ukey: 0}, to: st.shard0.shardName, _waitForDelete: true}));

assert.commandWorked(staleCollSh.insert(inserts));

//
// Test when the legacy batch exceeds the BSON object size limit
//

jsTest.log("Testing bulk insert (no COE) with large objects...");
resetColls();

var inserts = (function() {
    var data = 'x'.repeat(10 * 1024 * 1024);
    return [
        {ukey: 1, data: data},
        {ukey: 2, data: data},
        {ukey: -1, data: data},
        {ukey: -2, data: data}
    ];
})();

var staleMongosWithLegacyWrites = new Mongo(staleMongos.name);
staleMongosWithLegacyWrites.forceWriteMode('legacy');

staleCollSh = staleMongos.getCollection(collSh + "");
assert.eq(null, staleCollSh.findOne(), 'Collections should be empty');

assert.commandWorked(admin.runCommand(
    {moveChunk: collSh + "", find: {ukey: 0}, to: st.shard1.shardName, _waitForDelete: true}));
assert.commandWorked(admin.runCommand(
    {moveChunk: collSh + "", find: {ukey: 0}, to: st.shard0.shardName, _waitForDelete: true}));

staleCollSh.insert(inserts);
staleCollSh.getDB().getLastError();

st.stop();
})();

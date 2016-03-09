// Tests bulk inserts to mongos
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 2});

    var mongos = st.s;
    var staleMongos = st.s1;
    var config = mongos.getDB("config");
    var admin = mongos.getDB("admin");
    var shards = config.shards.find().toArray();

    for (var i = 0; i < shards.length; i++) {
        shards[i].conn = new Mongo(shards[i].host);
    }

    var collSh = mongos.getCollection(jsTestName() + ".collSharded");
    var collUn = mongos.getCollection(jsTestName() + ".collUnsharded");
    var collDi = shards[0].conn.getCollection(jsTestName() + ".collDirect");

    jsTest.log('Checking write to config collections...');
    assert.writeOK(admin.TestColl.insert({SingleDoc: 1}));
    assert.writeError(admin.TestColl.insert([{Doc1: 1}, {Doc2: 1}]));

    jsTest.log("Setting up collections...");

    assert.commandWorked(admin.runCommand({enableSharding: collSh.getDB() + ""}));
    st.ensurePrimaryShard(collSh.getDB() + "", shards[0]._id);

    assert.commandWorked(admin.runCommand({movePrimary: collUn.getDB() + "", to: shards[1]._id}));

    printjson(collSh.ensureIndex({ukey: 1}, {unique: true}));
    printjson(collUn.ensureIndex({ukey: 1}, {unique: true}));
    printjson(collDi.ensureIndex({ukey: 1}, {unique: true}));

    assert.commandWorked(admin.runCommand({shardCollection: collSh + "", key: {ukey: 1}}));
    assert.commandWorked(admin.runCommand({split: collSh + "", middle: {ukey: 0}}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: collSh + "", find: {ukey: 0}, to: shards[0]._id, _waitForDelete: true}));

    var resetColls = function() {
        assert.writeOK(collSh.remove({}));
        assert.writeOK(collUn.remove({}));
        assert.writeOK(collDi.remove({}));
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

    assert.writeOK(collSh.insert(inserts));
    assert.eq(2, collSh.find().itcount());

    assert.writeOK(collUn.insert(inserts));
    assert.eq(2, collUn.find().itcount());

    assert.writeOK(collDi.insert(inserts));
    assert.eq(2, collDi.find().itcount());

    jsTest.log("Bulk insert (no COE) with mongos error...");

    resetColls();
    var inserts = [{ukey: 0}, {hello: "world"}, {ukey: 1}];

    assert.writeError(collSh.insert(inserts));
    assert.eq(1, collSh.find().itcount());

    jsTest.log("Bulk insert (no COE) with mongod error...");

    resetColls();
    var inserts = [{ukey: 0}, {ukey: 0}, {ukey: 1}];

    assert.writeError(collSh.insert(inserts));
    assert.eq(1, collSh.find().itcount());

    assert.writeError(collUn.insert(inserts));
    assert.eq(1, collUn.find().itcount());

    assert.writeError(collDi.insert(inserts));
    assert.eq(1, collDi.find().itcount());

    jsTest.log("Bulk insert (no COE) with mongod and mongos error...");

    resetColls();
    var inserts = [{ukey: 0}, {ukey: 0}, {ukey: 1}, {hello: "world"}];

    var res = assert.writeError(collSh.insert(inserts));
    assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
    assert.eq(1, collSh.find().itcount());

    res = assert.writeError(collUn.insert(inserts));
    assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
    assert.eq(1, collUn.find().itcount());

    res = assert.writeError(collDi.insert(inserts));
    assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
    assert.eq(1, collDi.find().itcount());

    jsTest.log("Bulk insert (no COE) on second shard...");

    resetColls();
    var inserts = [{ukey: 0}, {ukey: -1}];

    assert.writeOK(collSh.insert(inserts));
    assert.eq(2, collSh.find().itcount());

    assert.writeOK(collUn.insert(inserts));
    assert.eq(2, collUn.find().itcount());

    assert.writeOK(collDi.insert(inserts));
    assert.eq(2, collDi.find().itcount());

    jsTest.log("Bulk insert to second shard (no COE) with mongos error...");

    resetColls();
    var inserts = [
        {ukey: 0},
        {ukey: 1},  // switches shards
        {ukey: -1},
        {hello: "world"}
    ];

    assert.writeError(collSh.insert(inserts));
    assert.eq(3, collSh.find().itcount());

    jsTest.log("Bulk insert to second shard (no COE) with mongod error...");

    resetColls();
    var inserts = [{ukey: 0}, {ukey: 1}, {ukey: -1}, {ukey: -2}, {ukey: -2}];

    assert.writeError(collSh.insert(inserts));
    assert.eq(4, collSh.find().itcount());

    assert.writeError(collUn.insert(inserts));
    assert.eq(4, collUn.find().itcount());

    assert.writeError(collDi.insert(inserts));
    assert.eq(4, collDi.find().itcount());

    jsTest.log("Bulk insert to third shard (no COE) with mongod and mongos error...");

    resetColls();
    var inserts =
        [{ukey: 0}, {ukey: 1}, {ukey: -2}, {ukey: -3}, {ukey: 4}, {ukey: 4}, {hello: "world"}];

    res = assert.writeError(collSh.insert(inserts));
    assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
    assert.eq(5, collSh.find().itcount());

    res = assert.writeError(collUn.insert(inserts));
    assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
    assert.eq(5, collUn.find().itcount());

    res = assert.writeError(collDi.insert(inserts));
    assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
    assert.eq(5, collDi.find().itcount());

    //
    // CONTINUE-ON-ERROR
    //

    jsTest.log("Bulk insert (yes COE) with mongos error...");

    resetColls();
    var inserts = [{ukey: 0}, {hello: "world"}, {ukey: 1}];

    assert.writeError(collSh.insert(inserts, 1));  // COE
    assert.eq(2, collSh.find().itcount());

    jsTest.log("Bulk insert (yes COE) with mongod error...");

    resetColls();
    var inserts = [{ukey: 0}, {ukey: 0}, {ukey: 1}];

    assert.writeError(collSh.insert(inserts, 1));
    assert.eq(2, collSh.find().itcount());

    assert.writeError(collUn.insert(inserts, 1));
    assert.eq(2, collUn.find().itcount());

    assert.writeError(collDi.insert(inserts, 1));
    assert.eq(2, collDi.find().itcount());

    jsTest.log("Bulk insert to third shard (yes COE) with mongod and mongos error...");

    resetColls();
    var inserts =
        [{ukey: 0}, {ukey: 1}, {ukey: -2}, {ukey: -3}, {ukey: 4}, {ukey: 4}, {hello: "world"}];

    // Last error here is mongos error
    res = assert.writeError(collSh.insert(inserts, 1));
    assert(!isDupKeyError(res.getWriteErrorAt(res.getWriteErrorCount() - 1).errmsg),
           res.toString());
    assert.eq(5, collSh.find().itcount());

    // Extra insert goes through, since mongos error "doesn't count"
    res = assert.writeError(collUn.insert(inserts, 1));
    assert.eq(6, res.nInserted, res.toString());
    assert.eq(6, collUn.find().itcount());

    res = assert.writeError(collDi.insert(inserts, 1));
    assert.eq(6, res.nInserted, res.toString());
    assert.eq(6, collDi.find().itcount());

    jsTest.log("Bulk insert to third shard (yes COE) with mongod and mongos error " +
               "(mongos error first)...");

    resetColls();
    var inserts =
        [{ukey: 0}, {ukey: 1}, {ukey: -2}, {ukey: -3}, {hello: "world"}, {ukey: 4}, {ukey: 4}];

    // Last error here is mongos error
    res = assert.writeError(collSh.insert(inserts, 1));
    assert(isDupKeyError(res.getWriteErrorAt(res.getWriteErrorCount() - 1).errmsg), res.toString());
    assert.eq(5, collSh.find().itcount());

    // Extra insert goes through, since mongos error "doesn't count"
    res = assert.writeError(collUn.insert(inserts, 1));
    assert(isDupKeyError(res.getWriteErrorAt(res.getWriteErrorCount() - 1).errmsg), res.toString());
    assert.eq(6, collUn.find().itcount());

    res = assert.writeError(collDi.insert(inserts, 1));
    assert(isDupKeyError(res.getWriteErrorAt(0).errmsg), res.toString());
    assert.eq(6, collDi.find().itcount());

    //
    // Test when WBL has to be invoked mid-insert
    //

    jsTest.log("Testing bulk insert (no COE) with WBL...");
    resetColls();

    var inserts = [{ukey: 1}, {ukey: -1}];

    var staleCollSh = staleMongos.getCollection(collSh + "");
    assert.eq(null, staleCollSh.findOne(), 'Collections should be empty');

    assert.commandWorked(admin.runCommand(
        {moveChunk: collSh + "", find: {ukey: 0}, to: shards[1]._id, _waitForDelete: true}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: collSh + "", find: {ukey: 0}, to: shards[0]._id, _waitForDelete: true}));

    assert.writeOK(staleCollSh.insert(inserts));

    //
    // Test when the objects to be bulk inserted are 10MB, and so can't be inserted
    // together with WBL.
    //

    jsTest.log("Testing bulk insert (no COE) with WBL and large objects...");
    resetColls();

    var data10MB = 'x'.repeat(10 * 1024 * 1024);
    var inserts = [
        {ukey: 1, data: data10MB},
        {ukey: 2, data: data10MB},
        {ukey: -1, data: data10MB},
        {ukey: -2, data: data10MB}
    ];

    staleCollSh = staleMongos.getCollection(collSh + "");
    assert.eq(null, staleCollSh.findOne(), 'Collections should be empty');

    assert.commandWorked(admin.runCommand(
        {moveChunk: collSh + "", find: {ukey: 0}, to: shards[1]._id, _waitForDelete: true}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: collSh + "", find: {ukey: 0}, to: shards[0]._id, _waitForDelete: true}));

    assert.writeOK(staleCollSh.insert(inserts));

    st.stop();

})();

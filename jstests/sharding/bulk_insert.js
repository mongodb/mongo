//
// Tests bulk inserts to mongodb
//

jsTest.log("Starting sharded cluster...")

var st = new ShardingTest({shards : 2,
                           mongos : 2,
                           verbose : 0,
                           separateConfig : 1})

st.stopBalancer();

var mongos = st.s;
var staleMongos = st.s1
var config = mongos.getDB("config");
var admin = mongos.getDB("admin");
var shards = config.shards.find().toArray()

for ( var i = 0; i < shards.length; i++) {
    shards[i].conn = new Mongo(shards[i].host);
}

var collSh = mongos.getCollection(jsTestName() + ".collSharded");
var collUn = mongos.getCollection(jsTestName() + ".collUnsharded");
var collDi = shards[0].conn.getCollection(jsTestName() + ".collDirect");

jsTest.log("Setting up collections...")

printjson(admin.runCommand({enableSharding : collSh.getDB() + ""}))
printjson(admin.runCommand({movePrimary : collSh.getDB() + "",
                            to : shards[0]._id}));
printjson(admin.runCommand({movePrimary : collUn.getDB() + "",
                            to : shards[1]._id}));

printjson(collSh.ensureIndex({ukey : 1}, {unique : true}));
printjson(collUn.ensureIndex({ukey : 1}, {unique : true}));
printjson(collDi.ensureIndex({ukey : 1}, {unique : true}));

printjson(admin.runCommand({shardCollection : collSh + "",
                            key : {ukey : 1}}))
printjson(admin.runCommand({split : collSh + "",
                            middle : {ukey : 0}}));
printjson(admin.runCommand({ moveChunk: collSh + "",
                             find: { ukey: 0 },
                             to: shards[0]._id,
                             _waitForDelete: true }));

var resetColls = function()
{
    collSh.remove({})
    assert.eq(null, collSh.getDB().getLastError());

    collUn.remove({})
    assert.eq(null, collUn.getDB().getLastError());

    collDi.remove({})
    assert.eq(null, collDi.getDB().getLastError());
}

var printPass = function(str)
{
    print(str);
    return str;
}

var isDupKeyError = function(err)
{
    return /dup key/.test(err + "");
}

jsTest.log("Collections created.");
st.printShardingStatus();

//
// BREAK-ON-ERROR
//

jsTest.log("Bulk insert (no ContinueOnError) to single shard...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : 1}]

collSh.insert(inserts);
assert.eq(null, printPass(collSh.getDB().getLastError()));
assert.eq(2, collSh.find().itcount());

collUn.insert(inserts);
assert.eq(null, printPass(collUn.getDB().getLastError()));
assert.eq(2, collUn.find().itcount());

collDi.insert(inserts);
assert.eq(null, printPass(collDi.getDB().getLastError()));
assert.eq(2, collDi.find().itcount());

jsTest.log("Bulk insert (no COE) with mongos error...")

resetColls();
var inserts = [{ukey : 0},
               {hello : "world"},
               {ukey : 1}]

collSh.insert(inserts);
assert.neq(null, printPass(collSh.getDB().getLastError()));
assert.eq(1, collSh.find().itcount());

jsTest.log("Bulk insert (no COE) with mongod error...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : 0},
               {ukey : 1}]

collSh.insert(inserts);
assert.neq(null, printPass(collSh.getDB().getLastError()));
assert.eq(1, collSh.find().itcount());

collUn.insert(inserts);
assert.neq(null, printPass(collUn.getDB().getLastError()));
assert.eq(1, collUn.find().itcount());

collDi.insert(inserts);
assert.neq(null, printPass(collDi.getDB().getLastError()));
assert.eq(1, collDi.find().itcount());

jsTest.log("Bulk insert (no COE) with mongod and mongos error...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : 0},
               {ukey : 1},
               {hello : "world"}]

collSh.insert(inserts);
var err = printPass(collSh.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(1, collSh.find().itcount());

collUn.insert(inserts);
var err = printPass(collUn.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(1, collUn.find().itcount());

collDi.insert(inserts);
var err = printPass(collDi.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(1, collDi.find().itcount());

jsTest.log("Bulk insert (no COE) on second shard...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : -1}]

collSh.insert(inserts);
assert.eq(null, printPass(collSh.getDB().getLastError()));
assert.eq(2, collSh.find().itcount());

collUn.insert(inserts);
assert.eq(null, printPass(collUn.getDB().getLastError()));
assert.eq(2, collUn.find().itcount());

collDi.insert(inserts);
assert.eq(null, printPass(collDi.getDB().getLastError()));
assert.eq(2, collDi.find().itcount());

jsTest.log("Bulk insert to second shard (no COE) with mongos error...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : 1}, // switches shards
               {ukey : -1},
               {hello : "world"}]

collSh.insert(inserts);
assert.neq(null, printPass(collSh.getDB().getLastError()));
assert.eq(3, collSh.find().itcount());

jsTest.log("Bulk insert to second shard (no COE) with mongod error...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : 1},
               {ukey : -1},
               {ukey : -2},
               {ukey : -2}]

collSh.insert(inserts);
assert.neq(null, printPass(collSh.getDB().getLastError()));
assert.eq(4, collSh.find().itcount());

collUn.insert(inserts);
assert.neq(null, printPass(collUn.getDB().getLastError()));
assert.eq(4, collUn.find().itcount());

collDi.insert(inserts);
assert.neq(null, printPass(collDi.getDB().getLastError()));
assert.eq(4, collDi.find().itcount());

jsTest
        .log("Bulk insert to third shard (no COE) with mongod and mongos error...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : 1},
               {ukey : -2},
               {ukey : -3},
               {ukey : 4},
               {ukey : 4},
               {hello : "world"}]

collSh.insert(inserts);
var err = printPass(collSh.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(5, collSh.find().itcount());

collUn.insert(inserts);
var err = printPass(collUn.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(5, collUn.find().itcount());

collDi.insert(inserts);
var err = printPass(collDi.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(5, collDi.find().itcount());

//
// CONTINUE-ON-ERROR
//

jsTest.log("Bulk insert (yes COE) with mongos error...")

resetColls();
var inserts = [{ukey : 0},
               {hello : "world"},
               {ukey : 1}]

collSh.insert(inserts, 1); // COE
assert.neq(null, printPass(collSh.getDB().getLastError()));
assert.eq(2, collSh.find().itcount());

jsTest.log("Bulk insert (yes COE) with mongod error...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : 0},
               {ukey : 1}]

collSh.insert(inserts, 1);
assert.neq(null, printPass(collSh.getDB().getLastError()));
assert.eq(2, collSh.find().itcount());

collUn.insert(inserts, 1);
assert.neq(null, printPass(collUn.getDB().getLastError()));
assert.eq(2, collUn.find().itcount());

collDi.insert(inserts, 1);
assert.neq(null, printPass(collDi.getDB().getLastError()));
assert.eq(2, collDi.find().itcount());

jsTest
        .log("Bulk insert to third shard (yes COE) with mongod and mongos error...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : 1},
               {ukey : -2},
               {ukey : -3},
               {ukey : 4},
               {ukey : 4},
               {hello : "world"}]

// Last error here is mongos error
collSh.insert(inserts, 1);
var err = printPass(collSh.getDB().getLastError());
assert.neq(null, err);
assert(!isDupKeyError(err));
assert.eq(5, collSh.find().itcount());

// Extra insert goes through, since mongos error "doesn't count"
collUn.insert(inserts, 1);
var err = printPass(collUn.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(6, collUn.find().itcount());

collDi.insert(inserts, 1);
var err = printPass(collDi.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(6, collDi.find().itcount());

jsTest.log("Bulk insert to third shard (yes COE) with mongod and mongos error "
           + "(mongos error first)...")

resetColls();
var inserts = [{ukey : 0},
               {ukey : 1},
               {ukey : -2},
               {ukey : -3},
               {hello : "world"},
               {ukey : 4},
               {ukey : 4}]

// Last error here is mongos error
collSh.insert(inserts, 1);
var err = printPass(collSh.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(5, collSh.find().itcount());

// Extra insert goes through, since mongos error "doesn't count"
collUn.insert(inserts, 1);
var err = printPass(collUn.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(6, collUn.find().itcount());

collDi.insert(inserts, 1);
var err = printPass(collDi.getDB().getLastError());
assert.neq(null, err);
assert(isDupKeyError(err));
assert.eq(6, collDi.find().itcount());

//
// Test when WBL has to be invoked mid-insert
//

jsTest.log("Testing bulk insert (no COE) with WBL...")

resetColls();
var inserts = [{ukey : 1},
               {ukey : -1}]

staleCollSh = staleMongos.getCollection(collSh + "");

staleCollSh.findOne();
printjson(admin.runCommand({moveChunk : collSh + "",
                            find : {ukey : 0},
                            to : shards[1]._id,
                            _waitForDelete: true}));
printjson(admin.runCommand({moveChunk : collSh + "",
                            find : {ukey : 0},
                            to : shards[0]._id,
                            _waitForDelete: true}));

staleCollSh.insert(inserts);
var err = printPass(staleCollSh.getDB().getLastError());
assert.eq(null, err);

//
// Test when the objects to be bulk inserted are 10MB, and so can't be inserted
// together with WBL.
//

jsTest.log("Testing bulk insert (no COE) with WBL and large objects...")

var data1MB = "x";
while (data1MB.length < 1024 * 1024)
    data1MB += data1MB;

var data10MB = "";
for ( var i = 0; i < 10; i++)
    data10MB += data1MB;

resetColls();
var inserts = [{ukey : 1,
                data : data10MB},
               {ukey : 2,
                data : data10MB},
               {ukey : -1,
                data : data10MB},
               {ukey : -2,
                data : data10MB}]

staleCollSh = staleMongos.getCollection(collSh + "");

staleCollSh.findOne();
printjson(admin.runCommand({moveChunk : collSh + "",
                            find : {ukey : 0},
                            to : shards[1]._id,
                            _waitForDelete: true}));
printjson(admin.runCommand({moveChunk : collSh + "",
                            find : {ukey : 0},
                            to : shards[0]._id,
                            _waitForDelete: true}));

staleCollSh.insert(inserts);
var err = printPass(staleCollSh.getDB().getLastError());
assert.eq(null, err);

st.stop()
